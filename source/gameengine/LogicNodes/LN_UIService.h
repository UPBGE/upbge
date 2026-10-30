/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_UIService.h
 *  \ingroup logicnodes
 *
 * Scene-local object UI routing. This service owns no rendering resources: it consumes the
 * existing camera transforms and overlay collection state, then resolves input against authored
 * object bounds once per Logic Nodes phase.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <array>

#include "KX_OverlayResource.h"
#include "LN_DenseIds.h"
#include "LN_Inventory.h"
#include "LN_Snapshot.h"
#include "LN_Types.h"
#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"

class KX_Camera;
class KX_GameObject;
class KX_Scene;
class LN_InputSnapshot;
class LN_Manager;
struct LN_InputBinding;
class LN_RuntimeTree;
enum class LN_InventoryOutcome : uint8_t;
enum class LN_InventoryLayout : uint8_t;

namespace blender {
struct Collection;
struct Object;
}

LN_UIResult LN_MapOverlayResult(KX_OverlayCollectionResult result);

/** Ray through a canvas position (top-left pixels) as `camera` renders it, honoring framing and
 * custom viewports. Shared by Object UI queries and UI Element hit testing. False outside the
 * camera's viewport, unless `beyond_view` (a drag held in that view goes on into another one). */
bool LN_ComputeCameraPointerRay(KX_Camera *camera,
                                float pointer_x,
                                float pointer_y,
                                float distance,
                                MT_Vector3 &r_from,
                                MT_Vector3 &r_to,
                                bool beyond_view = false);
/** The camera drawn at a canvas position (top-left pixels): the topmost camera viewport there
 * (split screen views, minimaps), else the active camera. Never the menu camera. While the
 * window is split, the mouse is player 0's: null outside their view. */
KX_Camera *LN_CameraUnderPointer(KX_Scene &scene, float pointer_x, float pointer_y);
/** A camera with its own viewport (split screen, minimap) draws one pixel more than its scene
 * viewport's width and height: the right and top edges are pixels it covers. */
int LN_CameraViewportDrawnExtra(const KX_Camera *camera);
/** Normalized screen position (0..1, top = 0) of a world position seen by `camera`. */
bool LN_ProjectWorldToScreen(KX_Camera *camera,
                             const MT_Vector3 &world_position,
                             MT_Vector3 &r_screen_position);

class LN_UIService {
 public:
  using MenuHandle = int32_t;

  struct ListenerKey {
    const LN_RuntimeTree *tree = nullptr;
    uint32_t query_index = LN_INVALID_INDEX;
    /** A UI Element listener is keyed by its object instead of a tree query. */
    const KX_GameObject *element = nullptr;
    /** A UI Element's player: an Any Player element has a listener per player. */
    int32_t user = 0;

    bool operator==(const ListenerKey &other) const
    {
      return tree == other.tree && query_index == other.query_index && element == other.element &&
             user == other.user;
    }
  };

  struct ListenerKeyHash {
    size_t operator()(const ListenerKey &key) const;
  };

  struct Registration {
    ListenerKey listener;
    KX_GameObject *object = nullptr;
    KX_Camera *camera = nullptr;
    MT_Vector3 ray_from = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 ray_to = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 bounds_min = MT_Vector3(-0.5f, -0.5f, -0.5f);
    MT_Vector3 bounds_max = MT_Vector3(0.5f, 0.5f, 0.5f);
    MT_Vector3 world_position = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Matrix3x3 world_orientation = MT_Matrix3x3::Identity();
    MT_Vector3 world_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
    MT_Vector3 screen_position = MT_Vector3(0.5f, 0.5f, 0.0f);
    float padding = 0.0f;
    float drag_threshold = 6.0f;
    int32_t user = 0;
    int32_t priority = 0;
    int32_t tab_order = 0;
    LN_UIPropagation propagation = LN_UIPropagation::Block;
    LN_UIInteractableRole role = LN_UIInteractableRole::Control;
    LN_InteractionDomain interaction_domain = LN_InteractionDomain::Any;
    std::string drop_group;
    /** UI Element Accept Categories, lowercase between commas (",weapon,shield,"); empty takes
     * every item. */
    std::string accept_categories;
    bool has_precomputed_hit = false;
    bool precomputed_hit = false;
    float precomputed_fraction = 1.0f;
    MT_Vector3 precomputed_point = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 precomputed_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
    bool enabled = true;
    bool capture_on_press = true;
    bool focus_on_press = true;
    bool focusable = true;
    /** Reachable by keyboard/gamepad navigation; false keeps focus click-only. */
    bool navigable = true;
    /** Accepts drops in addition to its own role (UI Elements can be dragged and be targets). */
    bool drop_target = false;
    /** A drag source that a click (or Accept) picks up until the next click puts it down. */
    bool click_pick_up = false;
  };

  struct InteractionState {
    LN_ObjectHandle object;
    MT_Vector3 point = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 normal = MT_Vector3(0.0f, 0.0f, 0.0f);
    bool valid = false;
    bool entered = false;
    bool over = false;
    bool exited = false;
    bool pressed = false;
    bool released = false;
    bool clicked = false;
    /** Secondary (right click, or its key or gamepad control on the focused control). */
    bool secondary_clicked = false;
    bool drag_started = false;
    bool dragging = false;
    bool drag_ended = false;
    bool captured = false;
    bool focused = false;
    /** Focused while the user plays with keyboard or gamepad; hidden while the mouse is used. */
    bool focus_visible = false;
    bool drop_accepted = false;
    bool drop_rejected = false;
    bool drag_cancelled = false;
    bool drop_entered = false;
    bool drop_over = false;
    bool drop_exited = false;
    bool dropped = false;
    /** A drag is over it that its Accept Categories refuse: letting go puts the item back. */
    bool drop_refused = false;
    LN_ObjectHandle source_object;
    LN_ObjectHandle target_object;
    /** Drag source: pointer hit point when the press started, and the accepted drop surface. */
    MT_Vector3 drag_origin = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 drop_point = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 drop_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
    /** A split stack's drag: how many of the grid item it carries (0: all of it). */
    int32_t drag_amount = 0;
  };

  /** Static settings of one UI Element object, read from its UI Element panel. */
  struct ElementSettings {
    std::string action;
    std::string drop_group;
    /** Lowercase between commas (",weapon,shield,"); empty takes every item. */
    std::string accept_categories;
    LN_UIPropagation pointer = LN_UIPropagation::Block;
    LN_UIElementFocus focus = LN_UIElementFocus::All;
    LN_UIElementAnchor anchor = LN_UIElementAnchor::None;
    /** Outside menus: the player who uses it, or every player (Any Player). In a menu it is the
     * menu's player's. */
    int32_t player = 0;
    bool any_player = false;
    int32_t tab_order = 0;
    int32_t priority = 0;
    float padding = 0.0f;
    float drag_threshold = 6.0f;
    float hover_scale = 1.0f;
    float press_scale = 1.0f;
    float look_time = 0.1f;
    bool draggable = false;
    bool follow_cursor = true;
    /** Drag Look Item Only: a Grid View slot stays in its cell and a copy of its item follows. */
    bool drag_item_only = false;
    bool drop_target = false;
    bool click_pick_up = false;
    /** Shown while a player points at the element: the element's descendant made from this
     * object (grid copies have their own), else that object itself. */
    blender::Object *highlight = nullptr;
    /** Shown while a dragged item its Accept Categories refuse is over it (found as highlight). */
    blender::Object *refused = nullptr;
    /** Shows tooltip_text while a player points at the element (found as highlight); a grid slot
     * only while it holds an item. */
    blender::Object *tooltip = nullptr;
    /** With the item's tokens in braces (ExpandItemTokens). */
    std::string tooltip_text = "{name}\n{description}";
    LN_UITooltipMode tooltip_mode = LN_UITooltipMode::InPlace;
    /** Beside: from the element to the tooltip, in the tooltip's parent space. */
    MT_Vector3 tooltip_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
    /** Widget: what the element does with a game property by itself. */
    LN_UIWidget widget = LN_UIWidget::None;
    /** The object with the property (null: the element), and the handle, check mark or text. */
    blender::Object *widget_object = nullptr;
    blender::Object *widget_part = nullptr;
    std::string widget_property;
    /** Bar: the owner's game property with the full value (empty: widget_max). */
    std::string widget_max_property;
    float widget_min = 0.0f;
    float widget_max = 1.0f;
    float widget_step = 0.1f;
    /** Where Up, Down, Left and Right go instead of the nearest element that way (by name). */
    std::array<blender::Object *, 4> navigation = {};
  };

  /** One UI Element event of the current frame, as reported by the On UI Event node. */
  struct ElementEvent {
    LN_UIElementEvent type = LN_UIElementEvent::Click;
    LN_ObjectHandle object;
    /** Drop: the UI Element that accepted it, else the game-world object under the pointer.
     * Drop Received: the dragged element. */
    LN_ObjectHandle target;
    MT_Vector3 point = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 normal = MT_Vector3(0.0f, 0.0f, 0.0f);
    std::string action;
    int menu_index = -1;
    int domain_rank = 0;
    int32_t priority = 0;
    /** The player it happened for: the element's menu's player, else its Player, or with Any
     * Player the one who used it. */
    int32_t user = 0;
    /** The grid item the element shows and its index in the grid's list (-1: not a grid slot);
     * the same for the target, when the target is a grid slot. */
    LN_Value item;
    int32_t index = -1;
    LN_Value target_item;
    int32_t target_index = -1;
    /** The target's Action when it is a UI Element: which grid or panel it was dropped on (Drop),
     * or came from (Drop Received). */
    std::string target_action;
    /** How many of the item the event is about: for drags (also Drop Received, Drop Enter and
     * Drop Exit, about the dragged item) what the drag carries, a split stack's part or all of
     * it; else the element's item's amount (0: no item). */
    int32_t amount = 0;
    /** What a Grid View's Inventory did with it: "Moved", "Stacked", "Combined", "Swapped",
     * "No Room", "Dropped"; empty when it did nothing. */
    std::string outcome;
    /** Drop Received: grids that handle moves took the drop but nothing changed (no room, the
     * same item with Stack off, two items with Swap off): the item went back (Denied sound). */
    bool inventory_unchanged = false;
    /** Drop with no accepting UI Element: look for a game-world target under the pointer. */
    bool needs_world_target = false;
    /** Drop Enter/Exit: the element takes the dragged item; Drop: a UI Element took it; Drop
     * Received: always. */
    bool accepted = false;
  };

  enum class ViewKind : uint8_t {
    Scroll = 0,
    Grid,
  };

  /** Object Grid View's Moving Items section: what the grid does with its list by itself. How
   * the list stacks, its Capacity, Keep Places and Recipes are the list's Inventory Rules
   * (ViewRegistration::rules), not the grid's. */
  struct GridInventory {
    /** Moves, stacks, swaps and combines items dropped between its slots and those of other grids
     * that handle moves (also another object's list: a chest and a bag), with the Inventory
     * node's Move To Slot rules. The rest needs it too. */
    bool handle_moves = false;
    /** A Game Input Map action held while a stack is picked up or dropped by Secondary Click: only
     * `split_amount` of it goes (0: half, rounded up). Empty: whole stacks. */
    std::string split_action;
    int32_t split_amount = 0;
    bool swap = true;
    /** A stack dragged out of the menu and let go over the player's view drops into the world,
     * all of it: `distance` in front of the list's object, or with `drop_at_pointer` on the
     * floor below the pointer. */
    bool drop_into_world = false;
    bool drop_at_pointer = false;
    /** Secondary Click on a stack drops it, all of it, `distance` in front of the list's object
     * on the floor. */
    bool drop_in_front = false;
    float distance = 1.5f;
    /** Drops float this high above where they land. */
    float hover_height = 0.0f;
    /** A text object that says whether a drag over one of the grid's slots would be taken, and
     * the two texts (ExpandItemTokens of the dragged item). Works without Handle Moves. */
    KX_GameObject *hint_text = nullptr;
    std::string accept_hint;
    std::string refuse_hint;
  };

  /** One query-only object container. Views move/cull runtime objects but own no render resource.
   */
  struct ViewRegistration {
    ListenerKey listener;
    ViewKind kind = ViewKind::Scroll;
    KX_GameObject *viewport = nullptr;
    KX_GameObject *content = nullptr;
    KX_Camera *camera = nullptr;
    MT_Vector3 ray_from = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 ray_to = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 bounds_min = MT_Vector3(-0.5f, -0.5f, -0.5f);
    MT_Vector3 bounds_max = MT_Vector3(0.5f, 0.5f, 0.5f);
    MT_Vector3 world_position = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Matrix3x3 world_orientation = MT_Matrix3x3::Identity();
    MT_Vector3 world_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
    MT_Vector3 min_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 max_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 wheel_step = MT_Vector3(0.0f, 0.0f, 0.0f);
    std::vector<KX_GameObject *> slots;
    std::vector<LN_Value> items;
    /** Grid View: `items` is the game property `list_property` of `list_object` (its Object and
     * Property inputs), so the grid can change the list itself. Null when Items gives it. */
    KX_GameObject *list_object = nullptr;
    std::string list_property;
    GridInventory inventory;
    /** Grid View: the Inventory Rules of the list (of `list_object`'s `list_property`) this
     * frame; the defaults for a list Items gives. */
    LN_InventoryRules rules;
    /** Grid View: objects named like the items, whose meshes are the slot icons: the library of
     * the list's rules, else the scene's Item Library. */
    blender::Collection *icon_library = nullptr;
    /** Grid View: with \a rows above 0, the first slot, copied into a rows × columns grid. */
    KX_GameObject *slot_template = nullptr;
    MT_Vector3 spacing = MT_Vector3(0.0f, 0.0f, 0.0f);
    /** Scrollbar: a bar and its handle (the bar's first mesh child), with their local bounds.
     * The handle shows the scroll position along the bar's longer side. */
    KX_GameObject *scrollbar = nullptr;
    KX_GameObject *scrollbar_handle = nullptr;
    MT_Vector3 scrollbar_min = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 scrollbar_max = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 handle_min = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 handle_max = MT_Vector3(0.0f, 0.0f, 0.0f);
    /** The viewport is a menu (overlay) object. */
    bool overlay = false;
    int32_t rows = 0;
    int32_t requested_first_index = 0;
    int32_t columns = 1;
    int32_t wheel_rows = 1;
    int32_t user = 0;
    int32_t priority = 0;
    float padding = 0.0f;
    bool enabled = true;
    /** Scroll View: the axes it scrolls. Grid View: horizontal fills the slots column by column
     * and scrolls by columns; vertical fills them row by row and scrolls by rows. */
    bool horizontal = false;
    bool vertical = true;
    bool cull_outside = true;
    bool focus_follow = true;
    bool show_counts = true;
    /** Each slot's text shows its item's name (the library object's "name" property), with
     * " xN" after it when Show Counts is on and it holds more than one. */
    bool show_names = false;
    /** Cells past the last item stay as empty slots (no item, nothing to drag): a drop there
     * puts the stack at the end, and gamepad players can reach an empty inventory. */
    bool show_empty = false;
    /** Grid View Filter: only items of these categories (comma separated, empty: all) whose name
     * contains `search` (any case, empty: all). The slots stay bound to the items' places in the
     * list. */
    std::string show_categories;
    std::string search;
    /** Grid View Use: a Game Input Map action that uses the highlighted slot's item, and comma
     * separated actions that use the first, second... slot's. */
    std::string use_action;
    std::string use_keys;
  };

  struct ViewState {
    bool valid = false;
    bool scrolled = false;
    bool items_rebound = false;
    bool at_start = true;
    bool at_end = true;
    int32_t first_visible_index = -1;
    int32_t last_visible_index = -1;
    /** Grid View: the page in view (from 1) and the page count (at least 1). */
    int32_t page = 1;
    int32_t page_count = 1;
    MT_Vector3 offset = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 normalized = MT_Vector3(0.0f, 0.0f, 0.0f);
  };

  struct MenuCommandResult {
    LN_UIResult result = LN_UIResult::RuntimeUnavailable;
    MenuHandle handle = 0;

    bool Succeeded() const
    {
      return result == LN_UIResult::Success || result == LN_UIResult::NoChange;
    }
  };

  LN_UIService(LN_Manager &manager, KX_Scene &scene);
  ~LN_UIService();

  /** `user` owns the menu: its UI Elements are that player's, and Back, Replace Top and Close Top
   * act on each player's own menus. `pauses_game`: the game is paused while the menu is open
   * (however it closes). `toggles`: if this collection's menu is open, it closes instead, with
   * the menus its player opened on top of it; the result is then Closed. */
  MenuCommandResult OpenMenu(const LN_RuntimeTree *owner_tree,
                             KX_GameObject *owner,
                             KX_Camera *camera,
                             blender::Collection *collection,
                             LN_UIMenuRouting routing,
                             std::string input_context,
                             LN_UIMenuOpenMode open_mode = LN_UIMenuOpenMode::Push,
                             LN_UIMenuPresentation presentation = LN_UIMenuPresentation::Screen,
                             LN_UIMenuBackBehavior back_behavior = LN_UIMenuBackBehavior::CloseMenu,
                             bool acquire_resource = true,
                             int32_t user = 0,
                             bool pauses_game = false,
                             bool toggles = false,
                             KX_GameObject *menu_owner = nullptr,
                             bool in_player_view = false);
  /** Top and All close `user`'s menus; Specific closes `handle` whoever owns it. */
  MenuCommandResult CloseMenu(MenuHandle handle,
                              LN_UIMenuCloseOperation operation = LN_UIMenuCloseOperation::Specific,
                              int32_t user = 0);
  void CloseMenusOwnedBy(KX_GameObject *owner);

  /** Start the once-per-frame UI pass: clear this frame's declarations before collection.
   * `input_snapshot` is the unfiltered frame input; the delta times held keys for repeating. */
  void BeginTick(LN_Event event,
                 uint64_t tick_index,
                 const LN_InputSnapshot *input_snapshot,
                 double unscaled_delta_time = 0.0);
  /** Device input that reaches Logic + Physics steps before the next UI frame: hide from them
   * what the UI will use (mouse buttons and the wheel over UI, the keys of an open menu). The UI
   * the last frame found decides, tested where the pointer and that UI are now. */
  void ConsumeInputBeforeResolve(const LN_InputSnapshot &sample);
  /** A Logic + Physics step does not resolve UI. Queries keep the latest frame's level states
   * (hover, focus, drag, scroll offset) without repeating that frame's one-tick events. */
  void BeginPassivePhase(LN_Event event, const LN_InputSnapshot *input_snapshot);
  /** Keep a dynamic listener alive even when its current target set is empty or invalid. */
  void DeclareInteractable(ListenerKey listener, int32_t user, bool enabled);
  void RegisterInteractable(Registration registration);
  void RegisterView(ViewRegistration registration);
  /** A Grid View's list property holds something other than a list: warns once. */
  void WarnGridListNotAList(KX_GameObject *object, const std::string &property);
  /** Resolve layout before interactable bounds are collected for the same Logic Nodes phase. */
  void ResolveViews();
  void ResolveInteractions();

  /** Index the scene's UI Element objects again when objects joined or left the scene. */
  void RefreshElementRegistry();
  bool HasElements() const;
  /** Anchor and register every UI Element for this frame (after RefreshElementRegistry). */
  void CollectElements();
  /** Register one UI Element for each player who uses it (ElementUsers); `registration` carries
   * its object, camera, player 0's pointer ray and its bounds. The mouse is player 0's: for the
   * others it hits nothing. */
  void RegisterElement(KX_GameObject *object,
                       const ElementSettings &settings,
                       Registration registration);
  /** After ResolveInteractions: drag-follow, hover/press scale and game-world drop targets. */
  void UpdateElementPresentation(double unscaled_delta_time);
  /** A menu's sound moments (On UI Sound's outputs): the highlight moves, a click, Back closes a
   * menu, a press that does nothing. The UI plays no sound itself: logic plays them. */
  using UISound = LN_UISound;
  /** A UI sound of this frame for On UI Sound: at most one a player, the most telling (Back,
   * then Denied, Accept, Move). */
  struct SoundEvent {
    UISound sound = UISound::None;
    int32_t user = 0;
    /** The UI Element it is for (none for Back), its Action, and the menu it is in or that Back
     * closed (its collection's name; empty outside menus). */
    LN_ObjectHandle object;
    std::string action;
    std::string menu;
  };
  /** This frame's UI sound number `occurrence` (0: the first) for `user` (-1: any player), or
   * null; `r_count` gets how many there are. */
  const SoundEvent *FindSoundEvent(int32_t user,
                                   size_t occurrence = 0,
                                   size_t *r_count = nullptr) const;
  /** Tests: a UI sound of this frame, as the UI frame would add it. */
  void AddSoundEventForTests(const SoundEvent &event);
  /** On UI Event runs its route for at most this many events of a frame: the topmost ones. */
  static constexpr size_t kMaxEventsPerNodeFrame = 64;
  /** Event number `occurrence` (0: the first) of `type` this frame for `action` (empty: any) and
   * `user` (-1: any player), or null; `r_count` gets how many events match. Events come topmost
   * element first. With `from_last`, 0 is the last of the topmost kMaxEventsPerNodeFrame. */
  const ElementEvent *FindElementEvent(LN_UIElementEvent type,
                                       const std::string &action,
                                       int32_t user = -1,
                                       size_t occurrence = 0,
                                       size_t *r_count = nullptr,
                                       bool from_last = false) const;
  /** What Scroll Object View's Show Item shows: an index in a Grid View's list, else the first
   * item there named `item` (or that is `object`); in a Scroll View, `object` in its content. */
  struct ScrollTarget {
    bool has_index = false;
    int32_t index = 0;
    std::string item;
    KX_GameObject *object = nullptr;
  };
  /** Scroll Object View: every Scroll or Grid View whose Viewport (or Content) is `viewport`
   * scrolls when it next lays out. `lines` is for Lines, `target` for Show Item. */
  void RequestViewScroll(KX_GameObject *viewport,
                         LN_UIViewScrollMode mode,
                         int32_t lines,
                         const ScrollTarget &target);
  void RequestViewScroll(KX_GameObject *viewport, LN_UIViewScrollMode mode, int32_t lines)
  {
    RequestViewScroll(viewport, mode, lines, ScrollTarget());
  }
  /** Show Message: adds `text` as the newest (last) line of `object`'s text (UITextObject), keeps
   * the newest `lines` and takes each away after `seconds` (0: only newer lines push it out). The
   * same text again moves to the end and starts over, adding its `count` to the line's: "Apple
   * x5" (nothing for 1), or the total where the text says {count}; `count` 0 adds no line
   * (nothing was picked up), -1 is a message without one. `user` >= 0: that player's copy of a
   * menu object (none when the player has not opened that menu). False when nothing shows it. */
  bool ShowMessage(KX_GameObject *object,
                   int32_t user,
                   const std::string &text,
                   double seconds,
                   int32_t lines,
                   int32_t count = -1);
  const InteractionState &GetInteractionState(const ListenerKey &listener) const;
  const ViewState &GetViewState(const ListenerKey &listener) const;
  bool GetBoundItem(KX_GameObject *object, LN_Value &r_item, int32_t &r_index) const;
  bool ObjectPassesViewClipping(KX_GameObject *object) const;
  /** The main-world camera player 0's pointer uses: the view under it (LN_CameraUnderPointer), or
   * while a press holds a world object, that press's camera so the drag goes on past its view. */
  KX_Camera *WorldPointerCamera(float pointer_x, float pointer_y) const;
  /** The same for one object: the held press's camera only for the object it holds; any other
   * object (a drop target) is seen in the view under the pointer (LN_CameraUnderPointer). */
  KX_Camera *WorldPointerCameraFor(const KX_GameObject *target,
                                   float pointer_x,
                                   float pointer_y) const;
  /** Player 0's mouse holds a press on a world object seen by `camera`: its pointer ray goes on
   * past that camera's view (LN_ComputeCameraPointerRay's `beyond_view`). */
  bool HoldsPointerInView(const KX_Camera *camera) const;
  /** `object` is a slot of a Grid View this frame. */
  bool IsGridSlot(const KX_GameObject *object) const;

  bool GetActionState(LN_UIAction action,
                      LN_UIActionState state,
                      int32_t user,
                      const std::string &context) const;
  LN_UIResult SetFocus(int32_t user, KX_GameObject *object, const std::string &context);
  KX_GameObject *GetFocus(int32_t user, const std::string &context) const;
  /** Take UI Control: outside menus, `user` alone uses the UI Elements of `root` and its
   * descendants, and that player's keys and gamepad stay on them (the first one is selected).
   * `take` false gives it back. ControlledByOtherPlayer when another player has it (or a part of
   * it); NoChange when nothing changes. */
  LN_UIResult TakeControl(KX_GameObject *root, int32_t user, bool take);
  /** The player who has the UI of `object` (it or an ancestor) by Take UI Control, or -1. */
  int32_t ControllerOf(KX_GameObject *object) const;
  /** Set Player Pointer: a cursor of `user` on the front face of `surface`, a flat object in the
   * game world. The left stick (or `move_action`, a vector action) moves it at `speed` surface
   * widths a second, Accept presses and Secondary right-clicks what it points at: that player's
   * UI Elements there get the mouse's events. Player 0's mouse moves it too, and then clicks as
   * usual. `cursor` is placed on the point, in front of the surface's children, and is shown while
   * the pointer is on. Disabled, the cursor is hidden and a drag is cancelled. `press_action`, an
   * action of the Game Input Map, replaces Accept (empty: Accept). */
  void SetPlayerPointer(int32_t user,
                        bool enabled,
                        KX_GameObject *surface,
                        KX_GameObject *cursor,
                        const std::string &move_action,
                        float speed,
                        const std::string &press_action = std::string());
  /** Set Player Pointer's View Center: `user` points with the middle of their camera view, as far
   * as `reach`, at their UI Elements in the game world (not through walls; the objects their
   * camera hangs from don't count). `press_action` (empty: Accept) presses what they aim at, and
   * goes to the UI only then. Several players aim at once, each with their own camera. */
  void SetPlayerAimPointer(int32_t user,
                           bool enabled,
                           float reach,
                           const std::string &press_action = std::string());
  /** Where `user`'s pointer points this frame (world space): on its surface, or where View
   * Center aims (the UI Element it hits, else the end of its reach); false while it is off. */
  bool GetPlayerPointerPoint(int32_t user, MT_Vector3 &r_point) const;
  /** `user`'s keys, gamepad or mouse are the UI's: a menu that takes their input is open, they
   * have Take UI Control, or a Set Player Pointer cursor on a surface is on (not View Center).
   * Mouse Look and Gamepad Look wait then. */
  bool UIUsesPlayerInput(int32_t user) const;
  /** Set Cursor Visibility's Only in Menus: while on, the system cursor shows only while a menu
   * uses player 0's mouse (UpdateMenuCursor, once a frame, also while the game is paused). */
  void SetCursorOnlyInMenus(bool on);
  void UpdateMenuCursor();
  /** The input context of `user`'s topmost menu that sets one. */
  const std::string &GetActiveInputContext(int32_t user = 0) const;

  /** The player whose menu holds `object`; outside menus the Player of the UI Element it is or
   * hangs from (a button's label), else 0 (Any Player too). */
  int32_t UserOfObject(KX_GameObject *object) const;
  /** The player whose menu holds `object`, or -1 outside menus. */
  int32_t MenuUserOfObject(KX_GameObject *object) const
  {
    const int index = MenuStackIndexForObject(object);
    return index >= 0 ? MenuUser(index) : -1;
  }
  /** Whose menu holds `object`: the Owner given to Open Object Menu, else the object whose logic
   * opened it (a menu opened from inside another menu inherits that menu's owner); null outside
   * menus. Per-player menus read their player's data through it. */
  KX_GameObject *MenuOwnerOfObject(KX_GameObject *object) const;
  /** The objects whose game property a widget changed this frame. */
  const std::vector<LN_ObjectHandle> &WidgetChangedOwners() const
  {
    return m_widgetChangedOwners;
  }
  /** Changes whenever a menu of any scene opens or closes. */
  static uint64_t MenuRevision()
  {
    return s_menuRevision;
  }

  void NotifyObjectRemoved(KX_GameObject *object);
  void NotifyRuntimeTreeRemoved(const LN_RuntimeTree *tree);
  void NotifyOverlayCollectionRemoved(blender::Collection *collection);
  void Clear();

  /** True when `owner` is a live overlay member of a collection this close will destroy. */
  bool ClosingWouldDestroyOwner(KX_GameObject *owner,
                                MenuHandle handle,
                                LN_UIMenuCloseOperation operation,
                                int32_t user = 0) const;
  /** True when `owner` is a live overlay member of `user`'s top menu that Replace Top will close,
   * and the new menu cannot take over the replaced menu's owner instead (it goes too). */
  bool ReplaceTopWouldDestroyOwner(KX_GameObject *owner, int32_t user = 0) const;

  size_t MenuCountForTests() const;
  size_t CaptureCountForTests() const;
  /** An open menu of `user` on top of the stack, without a collection (no scene needed): its
   * objects are `objects`. */
  MenuHandle AddMenuForTests(int32_t user,
                             LN_UIMenuRouting routing,
                             LN_UIMenuBackBehavior back_behavior,
                             const std::vector<KX_GameObject *> &objects = {});
  /** The pointer ray of a camera at a canvas position (LN_ComputeCameraPointerRay needs a running
   * engine). */
  using PointerRayFunction = std::function<bool(
      KX_Camera *camera, float x, float y, MT_Vector3 &r_from, MT_Vector3 &r_to)>;
  void SetPointerRayForTests(PointerRayFunction function);
  /** An object's local bounds (they come from its evaluated Blender object in the game). */
  using BoundsFunction = std::function<bool(
      KX_GameObject *object, MT_Vector3 &r_min, MT_Vector3 &r_max)>;
  void SetObjectBoundsForTests(BoundsFunction function);
  /** The camera of a player's view (the split screen views need a running scene). */
  using PlayerCameraFunction = std::function<KX_Camera *(int32_t user)>;
  void SetPlayerCameraForTests(PlayerCameraFunction function);
  /** The game world's first object on a ray and how far along (0..1) it is (the physics world
   * needs a running scene); `ignored`: objects whose descendants don't count either. */
  using WorldRayFunction = std::function<KX_GameObject *(const MT_Vector3 &from,
                                                         const MT_Vector3 &to,
                                                         const std::vector<KX_GameObject *> &ignored,
                                                         float &r_fraction)>;
  void SetWorldRayForTests(WorldRayFunction function);
  /** The Item Library object of an item, whatever the library (the scene's Item Library needs a
   * running scene). */
  using ItemLibraryFunction = std::function<KX_GameObject *(const std::string &item)>;
  void SetItemLibraryForTests(ItemLibraryFunction function);
  /** What player 0's pointer is over when a drag is let go where no UI Element takes it: other
   * UI, or another camera's view drawn over the player's (a minimap: it shows the world, but
   * letting go there is no drop into it); the player's own view with nothing under the pointer;
   * or a game-world object, hit at `point`. */
  enum class WorldDrop : uint8_t { None, View, Object };
  struct WorldDropHit {
    WorldDrop over = WorldDrop::None;
    KX_GameObject *object = nullptr;
    MT_Vector3 point = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 normal = MT_Vector3(0.0f, 0.0f, 1.0f);
  };
  /** The overlay UI, the views and the physics world need a running scene. */
  using WorldDropFunction = std::function<WorldDropHit(float pointer_x, float pointer_y)>;
  void SetWorldDropForTests(WorldDropFunction function);
  /** A drag's copy of a slot (the scene's replicas need a running scene): made for a slot, and
   * taken away. Item Only drags (Drag Look) make it too, then it follows the pointer. */
  using DragGhostMakeFunction = std::function<KX_GameObject *(KX_GameObject *slot)>;
  using DragGhostRemoveFunction = std::function<void(KX_GameObject *copy)>;
  void SetDragGhostForTests(DragGhostMakeFunction make, DragGhostRemoveFunction remove);

 private:
  struct MenuSession;
  struct ElementRecord;
  /** Where the pointer's input is the UI's, kept from a UI frame: an object and its hit shape (in
   * its own space), and the camera that shows it. Where the object is comes from the object
   * itself when the pointer is tested, so a menu that slides or a button on a moving object is
   * hit where it is now. */
  struct PointerSurface {
    LN_ObjectHandle object;
    LN_ObjectHandle camera;
    MT_Vector3 bounds_min = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 bounds_max = MT_Vector3(0.0f, 0.0f, 0.0f);
    float padding = 0.0f;
    /** A UI Element: hit at its size without the hover and press look (ElementHitTransform). */
    bool element = false;
    /** In the game world: walls in front of it keep the pointer from it. */
    bool main_world = false;
    /** Hit where the physics ray first hits the object (a node's Physics Surface hit test), not
     * by its bounds. */
    bool physics_hit = false;
    /** A view's bounds object may be invisible on purpose. */
    bool must_be_visible = true;
  };
  PointerSurface MakePointerSurface(const Registration &registration) const;
  /** `mouse` is over one of `surfaces`, with the cameras and objects as they are now. */
  bool PointerOverSurface(const std::vector<PointerSurface> &surfaces,
                          const LN_MouseSnapshot &mouse);
  /** This frame's Back may be the press that opened the menu (see MenuSession::opened_frame). */
  bool OpenedByThisBack(const MenuSession &session) const;
  struct ListenerDeclaration;
  struct ListenerPersistentState;
  struct PointerState;
  /** Player 0's mouse press held on a world object (not a keyboard or gamepad carry), or null. */
  const PointerState *HeldWorldPointer() const;
  struct Hit;
  struct ViewPersistentState;
  struct ViewVisibilityState;
  struct SlotBinding;
  struct RegistrationKey {
    ListenerKey listener;
    KX_GameObject *object = nullptr;

    bool operator==(const RegistrationKey &other) const
    {
      return listener == other.listener && object == other.object;
    }
  };
  struct RegistrationKeyHash {
    size_t operator()(const RegistrationKey &key) const;
  };

  static size_t PhaseIndex(LN_Event event);
  static bool IsValidListener(const ListenerKey &listener);
  static ListenerKey ElementListener(const KX_GameObject *object, const int32_t user)
  {
    return ListenerKey{nullptr, LN_INVALID_INDEX, object, user};
  }
  static uint8_t UserBit(const int32_t user)
  {
    return user >= 0 && user < LN_MAX_GAMEPADS ? uint8_t(1u << uint32_t(user)) : 0;
  }
  /** The players who use UI Element `object` at stack index `menu_index`, one bit each: the
   * menu's player; outside menus the player who took control of it, else its Player, or every
   * player with Any Player. */
  uint8_t ElementUsers(KX_GameObject *object,
                       int menu_index,
                       const ElementSettings &settings) const;
  /** Register a UI Element for `registration.user`. */
  void RegisterElementFor(ElementRecord &record,
                          const ElementSettings &settings,
                          int menu_index,
                          Registration registration);
  ElementRecord *FindElementRecord(const KX_GameObject *object);
  /** Where a UI Element is hit, as it is now: its place and size without its own or its parents'
   * hover and press look. */
  void ElementHitTransform(const ElementRecord &record,
                           MT_Vector3 &r_position,
                           MT_Vector3 &r_scale);
  void RemoveElementRecord(const KX_GameObject *object);
  void BuildElementEvents();
  void ApplyElementAnchor(ElementRecord &record, KX_Camera *camera);
  /** Fills a rejected drop's game-world target (an Object result) and says what it was let go
   * over. */
  WorldDrop ResolveWorldDropTarget(ElementEvent &event);
  /** What the pointer at (x, y) is over when `event`'s element is let go there (WorldDrop). */
  WorldDropHit WorldDropUnderPointer(const ElementEvent &event,
                                     float pointer_x,
                                     float pointer_y) const;
  WorldDropFunction m_worldDropForTests;
  /** Tab navigation order: Tab Order, then priority, then top-to-bottom, left-to-right. */
  static bool NavigationOrderLess(const Registration &a, const Registration &b);
  static bool RayIntersectsRegistration(const Registration &registration,
                                        float &r_fraction,
                                        MT_Vector3 &r_point,
                                        MT_Vector3 &r_normal);
  static bool RayIntersectsView(const ViewRegistration &registration);
  static bool IsDescendantOf(KX_GameObject *object, KX_GameObject *ancestor);
  /** The object, or an ancestor inside `root` (a view's content or slot), is the source of a drag
   * in progress: dragging a whole window that holds the view does not count. */
  bool IsBeingDragged(KX_GameObject *object, KX_GameObject *root) const;
  bool HasAnchoredAncestor(KX_GameObject *object);
  /** Stack index of `user`'s topmost "UI Only" menu, or -1: it blocks that player's menus below
   * it and, for player 0, the game-world UI. */
  int ModalFloorIndex(int32_t user) const;
  /** The player owning the menu at stack index `index` (0 outside menus). */
  int32_t MenuUser(int index) const;
  /** Stack index of `user`'s topmost menu, or -1. */
  int TopMenuIndex(int32_t user) const;
  /** Stack index of `user`'s topmost menu that takes that player's keys and buttons (Back,
   * keyboard and gamepad focus, Replace Top), or -1. A HUD opened as a menu does not: a "UI +
   * Game World" menu with nothing to select whose Back is not Close Menu. It leaves them to the
   * menus below it and the game, as if it were not a menu (split screen opens HUDs as menus to
   * give each player one). */
  int InputMenuIndex(int32_t user) const;
  static bool IsHudLayer(const MenuSession &session);
  /** Updates each menu's has_selectable from this frame's UI Elements. */
  void UpdateMenuSelectables();
  /** An object at stack index `index` is above its player's "UI Only" menus. Outside menus the
   * floor is `outside_user`'s. */
  bool MenuIndexPassesFloor(int index, int32_t outside_user) const;
  int MenuStackIndexForObject(KX_GameObject *object) const;
  /** `user`'s menus changed: the menu index is stale, that player's held direction must not keep
   * stepping through a menu it was not pressed in (other players' menus are not theirs), and the
   * highlight the change brings makes no Move sound. */
  void NoteMenuStackChanged(const int32_t user)
  {
    m_menuIndexValid = false;
    s_menuRevision++;
    if (const auto repeat = m_navigationRepeat.find(user); repeat != m_navigationRepeat.end()) {
      repeat->second.active = false;
    }
    if (user >= 0 && user < LN_MAX_GAMEPADS) {
      m_menuChangedFrame[size_t(user)] = m_frameIndex;
    }
  }
  void InvalidateMenuIndex()
  {
    m_menuIndexValid = false;
  }
  /** Focus is shown while the user's latest UI input came from keyboard or gamepad. */
  bool FocusShown(int32_t user) const;
  /** Keyboard and gamepad drive UI inside open menus, and outside them only after Set UI Focus
   * gave this user a focus there, so gameplay keys (Space, arrows, A) never press HUD buttons. */
  bool DirectedInputAllowed(int32_t user) const;
  bool IsRegisteredFocusable(int32_t user, KX_GameObject *object) const;
  /** `user` is whose "UI Only" menus block the object when it is outside every menu. */
  bool ObjectPassesModalRouting(KX_GameObject *object, int32_t user = 0) const;
  /** Keyboard/gamepad navigation and Accept of `user` stay on that player's top interactive
   * session. */
  bool ObjectAcceptsDirectedInput(KX_GameObject *object, int32_t user) const;
  bool SessionAllowsInteraction(KX_GameObject *object) const;
  bool SessionContainsObject(const MenuSession &session, KX_GameObject *object) const;
  bool SessionContainsHandle(const MenuSession &session, const LN_ObjectHandle &object) const;
  bool OtherSessionContainsHandle(MenuHandle excluded_handle, const LN_ObjectHandle &object) const;
  bool ContextMatches(const std::string &requested, int32_t user = 0) const;
  void ResolvePointerForUser(int32_t user, const std::vector<Hit> &hits);
  /** This frame's navigation step of `user`: a press, or a repeat of a held direction. Computed
   * once per frame; `r_from_press` is false for repeats. */
  bool NavigationForUser(int32_t user, LN_UIAction &r_action, bool &r_from_press);
  /** Hide the presses that triggered `action` from game logic: the UI used them. */
  void ConsumeActionPress(LN_UIAction action, int32_t user);
  enum class StickDirection : uint8_t { None, Up, Down, Left, Right };
  /** Which of a UI control's Input Map bindings a reading uses. */
  enum class BindingFilter : uint8_t { All, Mouse, NotMouse, NotWheel };
  /** A UI control's state from its bindings that pass `filter`. A press in `consumed` does not
   * count. */
  bool ActionBindingState(LN_UIAction action,
                          LN_UIActionState state,
                          int32_t user,
                          const LN_InputConsumption *consumed,
                          BindingFilter filter) const;
  /** The same for any action of the Game Input Map, by its index. */
  bool ActionIndexBindingState(int32_t action_index,
                               LN_UIActionState state,
                               int32_t user,
                               const LN_InputConsumption *consumed,
                               BindingFilter filter) const;
  bool StickBindingState(const LN_InputBinding &binding,
                         LN_UIActionState state,
                         int32_t user) const;
  /** Hide the presses of `action`'s bindings in `input` from game logic: the UI used them.
   * `whole_stroke`: also the hold and release of keys and mouse buttons. */
  void ConsumeBindingPresses(LN_UIAction action,
                             int32_t user,
                             const LN_InputSnapshot &input,
                             BindingFilter filter,
                             bool whole_stroke = false);
  void ConsumeActionIndexPresses(int32_t action_index,
                                 int32_t user,
                                 const LN_InputSnapshot &input,
                                 BindingFilter filter,
                                 bool whole_stroke = false);
  /** The pointer is over a UI Element with Block: hide every mouse button pressed in `input`
   * (its whole stroke) and its wheel from game logic. */
  void ConsumePointerOverUI(const LN_InputSnapshot &input);
  /** The mouse wheel steps in `input` (this frame's, or a sample before it) of the Scroll Up and
   * Scroll Down wheel bindings, up positive. */
  int32_t ScrollWheelSteps(const LN_InputSnapshot *input) const;
  /** This frame's scroll step of `user` from Scroll Up and Scroll Down's other bindings (the
   * right stick): 1 up, -1 down, 0 none. A held control repeats like navigation. */
  int32_t ScrollStepForUser(int32_t user);
  /** `focus_shown`: the focus was visible before this frame's input. While it is hidden, the
   * first key or button press only shows it, so nothing invisible is ever moved or pressed. */
  void ResolveNavigationForUser(int32_t user, bool focus_shown);
  /** Accept clicks the focused control; Secondary's keys and gamepad controls secondary-click
   * it. */
  void ResolveFocusedClickForUser(int32_t user, bool focus_shown, LN_UIAction action);
  /** The registration keys and gamepad reach on `object` for `user`: the topmost interactive one.
   */
  const Registration *DirectedRecipient(int32_t user, KX_GameObject *object) const;
  /** The element player 0's mouse points at, for keys to act on while their selection is hidden
   * (they last used the mouse; its hover look shows what they point at), or null.
   * `this_frame`: this frame's pointer is resolved (else last frame's hover). */
  KX_GameObject *PointedForKeys(int32_t user, bool this_frame) const;
  /** Picks up `source` into player 0's mouse, as a click on a Click to Pick Up element does: it
   * follows the pointer until a click (or Grab) puts it down where the pointer is. */
  void StartPointerPickUp(int32_t user, const Registration &source);
  /** What the last used device holds: player 0 used the mouse while their keys carry `pointer`'s
   * element, so the mouse holds it now, as if a click had picked it up (false when its element
   * is gone). MovePickUpToKeys: a navigation key while the mouse holds it, so the keys carry it
   * now, floating over the selection. Neither starts a new drag. */
  bool MoveCarryToMouse(int32_t user, PointerState &pointer);
  void MovePickUpToKeys(int32_t user, PointerState &pointer);
  /** The keys moved `user`'s selection: what the mouse holds goes to the keys (MovePickUpToKeys). */
  void HandPickUpToKeys(int32_t user);
  /** Where player 0's keys work now: the handle of the menu they are in, -1 for the UI outside
   * menus while it uses their input (Take UI Control, a pointer), 0 when the game has it. */
  int64_t PlayerZeroKeysPlace() const;
  /** Grab picks up the focused draggable element, which then floats over the focus; Grab or
   * Accept puts it down on the focused drop target (anything else puts it back), Back cancels.
   * With the mouse (selection hidden), Grab picks up what the mouse points at into the mouse
   * instead (StartPointerPickUp). True while it owns this user's Accept and Secondary presses. */
  bool ResolveCarryForUser(int32_t user, bool focus_shown);
  /** The object `user` carries with Grab, or null. */
  KX_GameObject *CarriedObject(int32_t user) const;
  /** A press, drag or carry of a player other than `user` holds `object` (an Any Player element):
   * `user` cannot take it too. */
  bool HeldByOtherUser(const KX_GameObject *object, int32_t user) const;
  void ReleaseListener(const ListenerKey &listener, bool synthesize_exit);
  void ReleaseView(const ListenerKey &listener);
  /** The template and its copies, made again when the template or grid layout changed. */
  std::vector<KX_GameObject *> TemplateSlots(const ViewRegistration &view);
  void RemoveTemplateCopies(ViewPersistentState &persistent);
  /** Removes the copies RemoveTemplateCopies queued, outside any view loop. */
  void RemoveQueuedTemplateCopies();
  /** Shows the item's icon and count on the slot; returns an icon to hide (no library mesh). */
  KX_GameObject *ShowSlotItem(const ViewRegistration &view,
                              ViewPersistentState &persistent,
                              size_t slot,
                              KX_GameObject *object,
                              const LN_Value &item,
                              KX_GameObject *icon,
                              KX_GameObject *text);
  /** What a slot's icon and count show, so meshes and text only change with the item. */
  struct SlotVisual;
  /** The same with its own record of what is shown. */
  KX_GameObject *ShowItemOn(const ViewRegistration &view,
                            SlotVisual &visual,
                            KX_GameObject *object,
                            const LN_Value &item,
                            KX_GameObject *icon,
                            KX_GameObject *text);
  /** A grid slot's icon (its first mesh child) and count text (its first text child), not its
   * Highlight, Refused, Tooltip or widget parts; with `r_picture`, also the slot and the children
   * a picture of it keeps (no parts, no UI Elements inside it). */
  void SlotParts(KX_GameObject *slot,
                 KX_GameObject *&r_icon,
                 KX_GameObject *&r_text,
                 std::set<KX_GameObject *> *r_picture);
  /** The slot's own parts that are no icon or count: Highlight, Refused, Tooltip, widget parts and
   * what is inside them. */
  static bool IsSlotPart(const ElementRecord *record, KX_GameObject *slot, KX_GameObject *child);

  /** Grid View slots that show another item than they last announced (ResolveViews), for On UI
   * Event Item Shown (BuildElementEvents). */
  struct CellShown;
  std::vector<CellShown> m_cellsShown;
  /** Copies of a dragged slot, by dragged slot. They are only pictures (never UI Elements, no
   * Highlight, Refused or Tooltip parts):
   * - Whole Element (Drag Look) with a split stack (Split Action): a copy left in the slot's cell
   *   shows what stays there while the slot follows the pointer; it goes when the drag ends.
   * - Item Only: a copy of the slot's icon and count (the copy of the slot itself hidden) follows
   *   the pointer while the slot stays in its cell, showing what is left; it goes the frame after
   *   the drag ends, once the grid shows where the item went. */
  struct DragGhost;
  std::unordered_map<const KX_GameObject *, DragGhost> m_dragGhosts;
  /** The copies and their children, kept out of the UI Elements. */
  std::unordered_set<const KX_GameObject *> m_dragGhostObjects;
  /** Makes the copy for `slot` at its place now, before it moves. */
  void StartDragGhost(KX_GameObject *slot);
  /** Item Only: makes the item's copy that follows the pointer, at the slot's place and size now;
   * the slot shows what is left at once. False (the whole slot follows then) when the slot is no
   * Grid View slot or its grid shows neither an icon nor a count. */
  bool StartDragPicture(KX_GameObject *slot);
  /** The copy following the pointer for an Item Only drag of `slot`, else null. */
  KX_GameObject *DragPictureOf(const KX_GameObject *slot) const;
  /** Puts the copy following the pointer at `position`, a little toward the camera so it draws
   * in front of the cells it passes over. */
  void PlaceDragPicture(const KX_GameObject *slot, const MT_Vector3 &position);
  /** What the cell of an Item Only drag's item shows meanwhile: the rest of a split stack, else
   * nothing. */
  static LN_Value ItemLeftInCell(const PointerState &drag);
  /** Each frame: the copy shows what the slot's cell holds now (`item`: the cell's item, or none;
   * hidden unless `active_cell`); the rest of the dragged stack while it is the dragged item's
   * cell. */
  void UpdateDragGhost(const ViewRegistration &view,
                       size_t slot,
                       KX_GameObject *object,
                       const LN_Value &item,
                       bool dragged_cell,
                       bool active_cell);
  /** The drag of `slot` ended: the slot shows what the copy showed, and the copy goes. */
  void EndDragGhost(KX_GameObject *slot);
  /** Copies whose drag ended without EndDragGhost (the slot gone, the drag cancelled elsewhere). */
  void SweepDragGhosts();
  /** Takes a copy away (queued, as template copies are). */
  void RemoveDragGhost(const LN_ObjectHandle &copy);
  DragGhostMakeFunction m_makeDragGhostForTests;
  DragGhostRemoveFunction m_removeDragGhostForTests;
  /** The pointer listener of a view's scrollbar. */
  static ListenerKey ScrollbarListener(const ViewRegistration &view)
  {
    return ListenerKey{view.listener.tree, view.listener.query_index, view.scrollbar};
  }
  /** While the press on the scrollbar is held: where it drags the view (0 start, 1 end).
   * `fraction` is the part of the content in view. */
  bool ScrollbarDragProgress(const ViewRegistration &view,
                             ViewPersistentState &persistent,
                             float fraction,
                             float &r_progress);
  /** Sizes and places the handle for `progress`, and registers the bar for the pointer. */
  void UpdateScrollbar(const ViewRegistration &view,
                       ViewPersistentState &persistent,
                       float fraction,
                       float progress);
  /** Puts the handle back where and how large it was authored. */
  void RestoreScrollbarHandle(ViewPersistentState &persistent);
  static bool RayHitsScrollbar(const ViewRegistration &view);
  void AcquireViewVisibility(const ListenerKey &listener, KX_GameObject *object);
  void ReleaseViewVisibility(const ListenerKey &listener, KX_GameObject *object);
  void CancelPointer(size_t phase, int32_t user, bool emit_event);
  /** At drag start: the grid item `object` shows, kept until the drag ends, its category, and how
   * many of it `user` takes (SplitAmount). */
  void RememberDragItem(PointerState &pointer, KX_GameObject *object, int32_t user) const;
  /** How many of `item` a pick-up or drop by `user` from a slot of `view` takes: 0 (all of it),
   * or the grid's Split Amount (0: half) while `user` holds its Split Action. */
  int32_t SplitAmount(const ViewRegistration *view, const LN_Value &item, int32_t user) const;
  /** The categories of a grid item, lowercase between commas: its own "category" value, else the
   * "category" property of its object in the Item Library of `view` (else the scene's). */
  std::string ItemCategories(const LN_Value &item, const ListenerKey &view) const;
  /** As ItemCategories, with `library` (null: the scene's Item Library). */
  std::string ItemCategoriesIn(const LN_Value &item, const blender::Collection *library) const;
  /** The Inventory Rules of `view`'s list, with the library the grid shows. */
  static LN_InventoryRules ListRules(const ViewRegistration &view);
  /** What the UI's inventory changes look up: stack limits, and categories as ItemCategoriesIn. */
  LN_InventoryLookup InventoryLookup() const;
  /** Grid View Use: `user` pressed the slot's grid's Use Action while the slot was their
   * highlight, or the Use Key of the slot's place in the grid (UseKeysReach), and the slot holds
   * an item. Takes the press from game logic. */
  bool SlotUsedBy(const ElementRecord &record, int32_t user);
  /** Use Keys reach a grid in menu `menu_index` (-1: none, a HUD's): the player is in that menu,
   * or in none and no other UI has their keys (Take UI Control, a pointer). */
  bool UseKeysReach(int32_t user, int menu_index) const;
  /** The Game Input Map action named for Grid View `setting` ("Use Action"...): warns once
   * when there is none. */
  int32_t GridAction(const std::string &name, const char *setting) const;
  /** The name players see: the item's own "name" value, else its Item Library object's "name"
   * property, else the item itself. */
  std::string ItemDisplayName(const ViewRegistration &view, const LN_Value &item) const;
  /** Grid View Filter: `item` is of `categories` (NormalizedCategories; empty: any) and its name
   * contains `search` (lower case; empty: any). An empty place never is. */
  bool FilterShows(const ViewRegistration &view,
                   const LN_Value &item,
                   const std::string &categories,
                   const std::string &search) const;
  /** The categories of what is dragged: of the dragged grid item, else the dragged object's own
   * "category" property, else that of the library object its "item" property names. */
  std::string DraggedItemCategories(KX_GameObject *object, const PointerState &pointer) const;
  /** Accept Categories allow dropping what `pointer` drags from `source` on `target`: the target
   * takes it, and a source with Accept Categories takes the target's item (a swap), unless the
   * drop goes into the target's list instead (DropGoesIntoList). The dragged item's categories
   * are looked up once, when first needed. */
  bool AcceptsDrop(const Registration &source, const Registration &target, PointerState &pointer)
      const;
  /** A slot that is a Drop Target with Accept Categories (equipment, a quick slot) takes `item`
   * of `view`'s list in a swap only when the item is of one of them. */
  bool SwapBackAllowed(bool drop_target,
                       const std::string &accept_categories,
                       const LN_Value &item,
                       const ListenerKey &view) const;
  /** Grid View Handle Moves: `item`, dragged from the slot `source`, dropped on the filled slot
   * `target` of another list's grid, goes into that list when it cannot change places with what
   * is there (MoveGridItem), as unequipping into a bag: both grids handle moves, the target grid
   * has more than one cell and its list has room (GridListHasRoom). */
  bool DropGoesIntoList(const KX_GameObject *source,
                        const KX_GameObject *target,
                        const LN_Value &item) const;
  /** `view`'s list (as read this frame) takes some of `item`: a stack of it with room (Stack on),
   * an empty place (Keep Places), or a new place within Capacity (0: no limit). */
  bool GridListHasRoom(const ViewRegistration &view, const LN_Value &item) const;
  /** The drag of `object` that keeps the item it showed when it started, or null. */
  const PointerState *FrozenDragOf(KX_GameObject *object) const;
  /** The player drags or carries something (a pointer drag, Click to Pick Up, Grab). */
  bool UserDrags(int32_t user) const;
  void ReleaseObjectState(KX_GameObject *object);
  void RestoreFocusAfterMenuClose(const MenuSession &session);
  void ApplyPendingInitialFocus();
  void RebaseFocusHistoryAfterMenuClose(const MenuSession &session);
  void RecomputeCoverage();
  void RefreshSessionMembership(MenuSession &session);
  void SnapshotSessionObjects(MenuSession &session);
  bool MenuSettingsMatch(const MenuSession &session,
                         KX_Camera *camera,
                         blender::Collection *collection,
                         LN_UIMenuRouting routing,
                         LN_UIMenuPresentation presentation,
                         LN_UIMenuBackBehavior back_behavior,
                         const std::string &input_context) const;
  /** The open menu of `collection` of `user` (-1: of any player), or 0. */
  MenuHandle FindSessionForCollection(const blender::Collection *collection,
                                      int32_t user = -1) const;
  /** Menus opened with Area Player's View: each kept in its player's split screen view. */
  void PlaceMenusInPlayerViews();
  /** \a to_window: back to the place it was authored for (a menu whose objects outlive it). */
  void PlaceMenuInPlayerView(MenuSession &session, bool to_window = false);
  bool OwnerLivesOnlyOnClosingCollection(KX_GameObject *owner, const MenuSession &session) const;
  /** What a player points at: the UI Element under player 0's mouse, or the focus a keyboard or
   * gamepad player sees. */
  struct Highlight {
    LN_ObjectHandle object;
    std::string action;
    /** The grid item it shows (0 when not a grid slot): a new item there is a new highlight. */
    size_t item_fingerprint = 0;
    int menu_index = -1;
    int domain_rank = 0;
    int32_t priority = 0;
    /** False while its event waits a frame for the leave event of another Action. */
    bool announced = true;
  };
  struct HighlightChange {
    int32_t user = 0;
    Highlight from;
    Highlight to;
    bool had = false;
    bool has = false;
  };
  /** After the players' input: each player's highlight, and the changes since the last frame for
   * the Highlight event. */
  void ResolveHighlights();
  /** Widgets change their game property: a click (Toggle, Selector), a drag on the track or
   * Left/Right while focused (Slider, Selector). Sends Changed events. */
  void ResolveWidgets();
  /** Left or Right of `user` on a focused Slider or Selector: it changes the value, not the
   * focus. */
  bool StepFocusedWidget(KX_GameObject *focused, LN_UIAction action, int32_t user);
  /** Player 0 typing into a Text Field: typed characters, Backspace, and Enter, Back or a click
   * elsewhere to finish (a Changed event). */
  void ResolveTextEditing();
  /** Where ElementPart found a part (or when it last missed). */
  struct PartCache {
    LN_ObjectHandle handle;
    uint64_t missed_frame = UINT64_MAX;
  };
  /** The element itself or its descendant made from `source` (copies have their own), else the
   * active object of that name; cached in `cache`. */
  KX_GameObject *ElementPart(KX_GameObject *element,
                             blender::Object *source,
                             PartCache &cache) const;
  /** Shows a widget's property on its part: check mark, handle position, option text. */
  void PresentWidget(ElementRecord &record);
  /** A Bar's fill: scaled along the bar from its start edge to show the value, moving toward a
   * new value over the element's Look Time. */
  void PresentBar(ElementRecord &record, float delta_time);
  /** The object whose game property a widget shows: its Widget Object, else the element itself
   * or its nearest parent that has the property (a bar on each enemy copy), else the element. */
  KX_GameObject *WidgetOwner(ElementRecord &record) const;
  /** The Highlight Objects of all elements: each shown while a player points at an element using
   * it. Refused Objects: each shown while a dragged item is refused over an element using it. */
  void PresentHighlights();
  struct HighlightFrame {
    bool shown = false;
    /** The first element using it, which remembers what was applied. */
    ElementRecord *record = nullptr;
  };
  /** PresentHighlights' scratch, kept to reuse its memory. */
  std::unordered_map<KX_GameObject *, HighlightFrame> m_highlightFrames;
  std::unordered_map<KX_GameObject *, HighlightFrame> m_refusedFrames;
  /** The Tooltip Objects of all elements: each shows what the element a player points at says,
   * the first player's when several point at elements sharing it. */
  void PresentTooltips();
  /** What a Tooltip Object shows, kept between frames: the element and item whose text it has,
   * and the text, place and visibility last applied. */
  struct TooltipState {
    LN_ObjectHandle tooltip;
    LN_ObjectHandle text_object;
    /** The text last written (empty: none, or emptied). */
    std::string text;
    LN_UITooltipMode mode = LN_UITooltipMode::InPlace;
    bool shown = false;
    bool visibility_known = false;
    bool text_known = false;
  };
  std::unordered_map<const KX_GameObject *, TooltipState> m_tooltips;
  struct TooltipFrame {
    /** The first element using it, and the one it shows (pointed at, with an item if it is a grid
     * slot) with the player pointing at it. */
    const ElementRecord *first = nullptr;
    const ElementRecord *shown = nullptr;
    int32_t user = 0;
  };
  /** PresentTooltips' scratch, kept to reuse its memory. */
  std::unordered_map<KX_GameObject *, TooltipFrame> m_tooltipFrames;
  /** Where a tooltip's or hint's text goes: `object` itself when it is a text object or has none,
   * else its first text child (remembered in `text_object`). */
  KX_GameObject *UITextObject(KX_GameObject *object, LN_ObjectHandle &text_object);
  /** Writes `text` in the "Text" game property of UITextObject. */
  void WriteUIText(KX_GameObject *object, LN_ObjectHandle &text_object, const std::string &text);
  /** The text there now (empty without one). */
  std::string CurrentUIText(KX_GameObject *object, LN_ObjectHandle &text_object);
  /** Empties what WriteUIText wrote, unless something else wrote there since (a shared info
   * line: a hint, a tooltip and game logic's messages). */
  void ClearUIText(KX_GameObject *object, LN_ObjectHandle &text_object, const std::string &written);

  /** Show Message's lines of one text object, the newest last. */
  struct MessageBox {
    LN_ObjectHandle object;
    LN_ObjectHandle text_object;
    struct Line {
      /** As given: the same text again is this line. */
      std::string text;
      /** Left to show (infinite: stays). */
      double seconds = 0.0;
      /** The counts given with it, added up (-1: a message without one). */
      int64_t count = -1;
    };
    std::vector<Line> lines;
    /** What it wrote last. */
    std::string written;
  };
  std::vector<MessageBox> m_messages;
  /** Counts down the messages' lines and writes what is left (every UI frame, paused too). */
  void PresentMessages(double delta_time);
  /** The text of a message box's lines. */
  static std::string MessageText(const MessageBox &box);

  /** Grid View Inventory: a drop between grids that handle moves moves the item (Move To Slot),
   * a stack dragged into the game world or Secondary-clicked drops there; each event tells what
   * happened in `outcome`. After the game-world targets of drops are known. */
  void ApplyGridInventory();
  /** Grid View Hint Text: what a drag over a grid slot would do, on the grid's text object; the
   * text empties when the drag leaves. */
  void PresentGridHints();
  /** The Grid View `object` is a slot of this frame, or null. */
  const ViewRegistration *GridOfSlot(const KX_GameObject *object) const;
  /** How the grid's list keeps its slots (Keep Places). */
  static LN_InventoryLayout LayoutOf(const ViewRegistration &view);
  /** The grid of a slot when it handles moves of its own list (Handle Moves), or null. */
  const ViewRegistration *HandlingGrid(const LN_ObjectHandle &slot) const;
  /** The slot's grid drops a stack let go over the player's view in front of its list's object
   * (Drop Into World without At Pointer): over nothing is a drop too. */
  bool DropsInFront(const LN_ObjectHandle &slot) const;
  /** Where a stack dropped in front of `view`'s list object lands: `distance` ahead on the
   * ground (closer when a wall is in the way), or the first free spot beside it when a pickup
   * lies there, plus Hover Height. */
  MT_Vector3 InFrontDropPosition(const ViewRegistration &view) const;
  /** Adds where the pickups within `radius` of `center` (along the ground) are: game-world
   * objects with an "item" game property, and the drops queued for the next step. */
  void NearbyPickups(const MT_Vector3 &center,
                     float radius,
                     std::vector<MT_Vector3> &r_positions) const;
  /** The object named `item` in `library` (null: the scene's Item Library), or null. */
  KX_GameObject *FindLibraryItem(const blender::Collection *library,
                                 const std::string &item) const;
  ItemLibraryFunction m_itemLibraryForTests;
  /** The list of a grid with a list property, read now; false (with a warning) when the property
   * holds something other than a list. A missing property is an empty list. */
  bool ReadGridList(const ViewRegistration &view, std::vector<LN_Value> &r_list);
  /** Writes a grid's list back; false (with a warning) when it cannot. */
  bool WriteGridList(const ViewRegistration &view, const std::vector<LN_Value> &list);
  /** The index of `item` in `list`: `index` while it is still there, else where it is now (-1:
   * gone), as a dragged slot finds its item. */
  static int32_t FindGridItem(const std::vector<LN_Value> &list,
                              int32_t index,
                              const LN_Value &item);
  /** The slot of `source` that Drop Received `event` names (its target) dropped on its slot of
   * `target`, with `target`'s rules; writes the lists. None when nothing changed. On a filled
   * slot where nothing else can happen (no swap, a full stack, a split part), a stack from
   * another list goes into the target's list (DropGoesIntoList) and a part split off inside one
   * list takes the nearest free place (as Split Stack). */
  LN_InventoryOutcome MoveGridItem(const ViewRegistration &target,
                                   const ViewRegistration &source,
                                   const ElementEvent &event);
  /** Takes the stack at `index` of `view`'s list (`amount` of it, 0: all) and drops a copy of its
   * Item Library object at `position` holding that many, on the next Logic + Physics step; false
   * (nothing changes) when it cannot. */
  bool DropGridStack(const ViewRegistration &view,
                     int32_t index,
                     const LN_Value &item,
                     const MT_Vector3 &position,
                     int32_t amount);
  /** Where something dropped at `point` lands: on the first solid surface below it (from `normal`
   * a little away from what it hit), not `ignored` or its children; `point` when there is none. */
  MT_Vector3 DropFloorPoint(const MT_Vector3 &point,
                            const MT_Vector3 &normal,
                            KX_GameObject *ignored) const;
  /** The first solid game-world object on the segment, not `ignored` or its children (nor shapes
   * the segment starts in), and where it is hit. */
  KX_GameObject *SolidWorldHit(const MT_Vector3 &from,
                               const MT_Vector3 &to,
                               KX_GameObject *ignored,
                               MT_Vector3 &r_point) const;
  /** What each Hint Text object shows, by object (only those a grid wrote). */
  struct HintState {
    LN_ObjectHandle hint;
    LN_ObjectHandle text_object;
    std::string text;
    /** What the text was made for (the slot, the dragged item, taken or not): made again only
     * when it changes. */
    size_t key = 0;
    bool key_known = false;
    bool used = false;
  };
  std::unordered_map<const KX_GameObject *, HintState> m_hints;
  /** Grid inventory mistakes already reported (once per kind and name). */
  mutable std::unordered_set<std::string> m_gridWarnings;
  /** A bar's full fill (see ElementRecord), by the bar's and fill's authored names: a copy made
   * from a bar that shows a value has a shrunk fill, so copies take the first one seen. */
  struct BarShape {
    MT_Vector3 base;
    MT_Vector3 scale;
    MT_Vector3 fill_scale;
    float start = 0.0f;
    float end = 0.0f;
    int axis = 0;
  };
  std::unordered_map<std::string, BarShape> m_barShapes;
  /** Bar setup mistakes already reported, once per authored bar. */
  std::unordered_set<std::string> m_barWarnings;
  /** Each player's UI sound of this frame, for On UI Sound (m_soundEvents). */
  void CollectUISounds();
  /** Back closes `session`: its player's Back sound, told before the menu is gone. */
  void NoteBackSound(const MenuSession &session);
  /** The name of the menu at stack index `menu_index` (its collection's), empty for none. */
  std::string MenuNameAt(int menu_index) const;
  bool TryConsumeCancelToken();
  void ProcessSemanticCancel();
  /** Back of a player other than 0, who has only a gamepad: it acts on that player's own top
   * menu. */
  void ProcessPlayerBack(int32_t user);
  /** Pauses the game while a menu opened with Pause Game is open in this scene, and resumes it
   * when the last such menu of any scene closes (unless it was paused before they opened). */
  void UpdateMenuPause();
  /** This scene's menus stop holding the pause; `resume`: resume the game if they paused it. */
  void ReleaseMenuPause(bool resume);
  MenuHandle AllocateMenuHandle();

  LN_Manager &m_manager;
  KX_Scene &m_scene;
  std::unordered_map<MenuHandle, MenuSession> m_sessions;
  std::vector<MenuHandle> m_menuStack;
  /** Protect final overlay teardown from object-removal callbacks that can re-enter CloseMenu. */
  std::unordered_set<MenuHandle> m_closingMenus;
  std::vector<Registration> m_registrations;
  std::unordered_map<ListenerKey, ListenerDeclaration, ListenerKeyHash> m_listenerDeclarations;
  /** Constant-time duplicate suppression. Clear retains buckets across UI phases. */
  std::unordered_set<ListenerKey, ListenerKeyHash> m_registeredListeners;
  std::unordered_set<RegistrationKey, RegistrationKeyHash> m_registeredTargets;
  std::vector<ViewRegistration> m_viewRegistrations;
  /** Scroll Object View requests, applied when their view next lays out (dropped if none does
   * within two frames). */
  struct ViewScrollRequest {
    LN_ObjectHandle viewport;
    LN_UIViewScrollMode mode = LN_UIViewScrollMode::Top;
    int32_t lines = 0;
    bool has_index = false;
    int32_t index = 0;
    std::string item;
    LN_ObjectHandle object;
    uint64_t frame = 0;
    /** A view laid out with it this frame: it goes once every view had its turn. */
    bool used = false;
  };
  std::vector<ViewScrollRequest> m_viewScrollRequests;
  /** Changes whenever a view or its slot list changes; IsGridSlot indexes the slots per version.
   */
  uint64_t m_viewSlotsVersion = 0;
  mutable uint64_t m_gridSlotsVersion = UINT64_MAX;
  mutable std::unordered_set<const KX_GameObject *> m_gridSlots;
  std::unordered_map<ListenerKey, InteractionState, ListenerKeyHash> m_currentStates;
  std::unordered_map<ListenerKey, ViewState, ListenerKeyHash> m_currentViewStates;
  std::unordered_map<ListenerKey, ViewPersistentState, ListenerKeyHash> m_viewStates;
  std::unordered_map<KX_GameObject *, ViewVisibilityState> m_viewVisibilityStates;
  std::unordered_map<KX_GameObject *, SlotBinding> m_slotBindings;
  std::vector<LN_ObjectHandle> m_templateCopiesToRemove;
  std::unordered_map<ListenerKey, ListenerPersistentState, ListenerKeyHash> m_listenerStates[3];
  std::unordered_map<int32_t, PointerState> m_pointerStates[3];
  std::unordered_map<int32_t, LN_ObjectHandle> m_focusByUser;
  /** Users whose latest UI input came from keyboard or gamepad; mouse use removes them. */
  std::unordered_set<int32_t> m_directedUsers;
  /** Users that Set UI Focus gave a focus while no menu was open. */
  std::unordered_set<int32_t> m_focusOutsideMenuUsers;
  /** Menu stack index of every object of an open menu, rebuilt lazily after menu changes. */
  mutable std::unordered_map<const KX_GameObject *, int> m_menuIndexByObject;
  mutable bool m_menuIndexValid = false;
  /** Left and right stick direction of each gamepad in this frame and the previous one. */
  std::array<std::array<StickDirection, 2>, LN_MAX_GAMEPADS> m_stickDirection = {};
  std::array<std::array<StickDirection, 2>, LN_MAX_GAMEPADS> m_previousStickDirection = {};
  std::array<std::array<bool, 2>, LN_MAX_GAMEPADS> m_stickWaitsForCentre = {};
  struct NavigationRepeat {
    LN_UIAction action = LN_UIAction::Next;
    bool active = false;
    double held = 0.0;
    double next_step = 0.0;
    /** The UI frame it was last updated in: a gap means the hold was not watched. */
    uint64_t frame = 0;
  };
  struct FrameNavigation {
    LN_UIAction action = LN_UIAction::Next;
    bool has_action = false;
    bool from_press = false;
  };
  std::unordered_map<int32_t, NavigationRepeat> m_navigationRepeat;
  std::unordered_map<int32_t, FrameNavigation> m_frameNavigation;
  std::unordered_map<int32_t, NavigationRepeat> m_scrollRepeat;
  std::unordered_map<int32_t, int32_t> m_frameScroll;
  double m_frameDelta = 0.0;
  /** Counts UI frames (BeginTick calls). */
  uint64_t m_frameIndex = 0;
  /** The pointer's clicks were the UI's in the last frame: it was over UI that blocks it, or
   * carried what a click picked up. */
  bool m_pointerBlockedByUI = false;
  /** A disabled UI Element with Block under player 0's pointer this frame: no control, but the
   * pointer's buttons and wheel are the UI's there. */
  LN_ObjectHandle m_disabledBlocker;
  /** The pointer is over a UI Element with Block (enabled or not) this frame: every mouse button
   * and the wheel are the UI's, not game logic's. */
  bool m_pointerOverBlockingUI = false;
  /** Where player 0's pointer input is the UI's, as the last frame found it: the UI Elements with
   * Block (all mouse buttons and the wheel) and the Grid and Scroll Views (the wheel). Input that
   * reaches Logic + Physics steps before the next frame is tested against them at its own
   * pointer position, with the objects where they are then. */
  std::vector<PointerSurface> m_blockSurfaces;
  std::vector<PointerSurface> m_wheelSurfaces;
  PointerRayFunction m_pointerRayForTests;
  /** Users that had a control keys and gamepad can reach in the last frame. */
  std::unordered_set<int32_t> m_directedTargetUsers;
  std::unordered_set<int32_t> m_viewNavigationHandledUsers;
  /** UI Element registry, rebuilt only when the scene's object list changes. */
  std::vector<ElementRecord> m_elements;
  std::unordered_map<const KX_GameObject *, size_t> m_elementIndex;
  uint64_t m_elementsGeneration = UINT64_MAX;
  std::vector<ElementEvent> m_elementEvents;
  /** The topmost UI Element under each player's pointer this frame (not the one being dragged):
   * player 0's mouse, or a Set Player Pointer cursor. */
  std::array<LN_ObjectHandle, LN_MAX_GAMEPADS> m_pointerHovers;
  /** m_pointerHovers of the last frame (navigation resolves before this frame's pointer). */
  std::array<LN_ObjectHandle, LN_MAX_GAMEPADS> m_previousPointerHovers;
  /** This frame, the users whose Grab press put down what the pointer carried (one bit each): it
   * picks nothing up again. */
  uint8_t m_pointerGrabUsed = 0;
  /** Where player 0 last moved, clicked or scrolled the mouse for the UI (PlayerZeroKeysPlace, 0
   * for nowhere): only there do keys act on what the mouse points at (PointedForKeys). Not on a
   * cursor a menu just opened over, nor after Mouse Look moved the mouse. */
  int64_t m_mouseUsedForKeysIn = 0;
  /** PlayerZeroKeysPlace as last seen, and for how long (seconds of UI frames) it stayed: mouse
   * input in the first moments after it changed does not count (the system can report where its
   * pointer is when a menu shows the cursor). */
  int64_t m_keysPlace = 0;
  double m_keysPlaceAge = 0.0;
  /** Set Player Pointer: a player's cursor on a surface in the game world. */
  struct PlayerPointer {
    bool enabled = false;
    /** View Center: the middle of the player's camera view points, as far as `reach`; no surface
     * or cursor. */
    bool aim = false;
    float reach = 3.0f;
    LN_ObjectHandle surface;
    LN_ObjectHandle cursor;
    /** A vector action that moves it; empty: the left stick. */
    std::string move_action;
    float speed = 1.0f;
    /** The action that presses (its left button); empty: Accept. */
    std::string press_action;
    bool warned_press_action = false;
    /** View Center: last frame it aimed at a UI Element with Block, so its presses were the UI's
     * (input before the next frame is judged by it, ConsumeInputBeforeResolve). */
    bool aim_blocked = false;
    /** This frame View Center aims at a disabled UI Element with Block. */
    bool aim_disabled_block = false;
    /** Switched between a surface and View Center: the next UI frame lets go of what it held. */
    bool mode_changed = false;
    /** The point on the surface: its local coordinates along the surface's two long axes. */
    float local_u = 0.0f;
    float local_v = 0.0f;
    bool placed = false;
    /** Player 0: the mouse moved it last, and clicks as usual (until the stick moves it). */
    bool mouse_mode = false;
    /** The cursor was shown by the pointer (hidden again when it stops). */
    bool cursor_shown = false;
    /** How far in front of the surface the cursor floats (kept while a press is held). */
    float lift = 0.0f;
    bool warned_action = false;
    /* This frame. */
    /** On, on a surface, and the player has no menu that takes their keys and gamepad. */
    bool active = false;
    MT_Vector3 point = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 ray_from = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 ray_to = MT_Vector3(0.0f, 0.0f, 0.0f);
    /** Where it is on the surface, 1000 units per surface width (the drag threshold's pixels),
     * and its move since the last frame. */
    MT_Vector3 position = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 delta = MT_Vector3(0.0f, 0.0f, 0.0f);
  };
  std::array<PlayerPointer, LN_MAX_GAMEPADS> m_playerPointers;
  /** The UI frame (m_frameIndex) the pointers were last moved in. */
  uint64_t m_pointersFrame = UINT64_MAX;
  /** Moves each player's pointer once a frame and places its cursor (before the UI Elements are
   * registered: player 0 switches between the mouse and the stick there). */
  void UpdatePlayerPointers();
  /** `user`'s pointer is on this frame (outside menus that take their input). */
  bool PointerActive(const int32_t user) const
  {
    return user >= 0 && user < LN_MAX_GAMEPADS && m_playerPointers[size_t(user)].active;
  }
  /** ...and points by itself, not with the mouse: its rays replace the mouse's. */
  bool VirtualPointerActive(const int32_t user) const
  {
    return PointerActive(user) && !m_playerPointers[size_t(user)].mouse_mode;
  }
  /** ...with the middle of the player's view (View Center). */
  bool AimPointerActive(const int32_t user) const
  {
    return PointerActive(user) && m_playerPointers[size_t(user)].aim;
  }
  /** The state of `user`'s pointer button: its Press Action (Accept when empty). A surface
   * pointer reads its keys and gamepad (the mouse clicks as usual), View Center the mouse too. */
  bool PointerButtonState(int32_t user, LN_UIActionState state);
  /** Hide the presses of `user`'s pointer button in `input` from game logic (whole strokes). */
  void ConsumePointerButton(int32_t user, const LN_InputSnapshot &input);
  /** The Input Map action of `user`'s pointer button, or -1 (warned once). */
  int32_t PointerButtonAction(int32_t user);
  /** Set Cursor Visibility's Only in Menus is on, and whether it showed the cursor last (-1: not
   * applied yet). */
  bool m_cursorOnlyInMenus = false;
  int m_menuCursorShown = -1;
  /** The stick or Move Action of `user`'s pointer this frame (x right, y up, at most 1 long). */
  MT_Vector3 PointerMove(int32_t user, PlayerPointer &pointer);
  /** The cursors pointers show: the UI's world raycasts pass through them. */
  std::vector<KX_GameObject *> ShownPointerCursors() const;
  KX_Camera *PlayerViewCamera(int32_t user) const;
  bool LocalBounds(KX_GameObject *object, MT_Vector3 &r_min, MT_Vector3 &r_max) const;
  BoundsFunction m_objectBoundsForTests;
  PlayerCameraFunction m_playerCameraForTests;
  WorldRayFunction m_worldRayForTests;
  /** Take UI Control: a player has the UI of an object and its descendants. */
  struct UIControl {
    LN_ObjectHandle root;
    int32_t user = 0;
    /** Its first element is still to be selected for the player. */
    bool focus_pending = true;
  };
  std::vector<UIControl> m_controls;
  bool HoldsControl(int32_t user) const;
  /** The UI frame Take UI Control selected each player's first element in: the press that took
   * the UI (Use is often Accept too) must not also press it. */
  std::array<uint64_t, LN_MAX_GAMEPADS> m_controlFocusFrame = {};
  /** A disabled UI Element was under player 0's mouse when it was pressed (Denied sound): the
   * first such element, for On UI Sound. */
  bool m_pressedDisabledElement = false;
  SoundEvent m_pressedDisabled;
  std::vector<LN_ObjectHandle> m_widgetChangedOwners;
  /** The Text Field player 0 types into, the frame it started, Back pressed while typing, and
   * Backspace's hold time and next repeat. */
  LN_ObjectHandle m_editingText;
  uint64_t m_editingTextFrame = 0;
  bool m_textEditCancelled = false;
  double m_backspaceHeld = 0.0;
  double m_backspaceNext = 0.0;
  /** Left/Right presses for focused Sliders and Selectors this frame, and the player who pressed
   * last. */
  struct WidgetSteps {
    int32_t steps = 0;
    int32_t user = 0;
  };
  std::unordered_map<KX_GameObject *, WidgetSteps> m_widgetSteps;
  /** This frame's UI sounds for On UI Sound (CollectUISounds): at most one a player. */
  std::vector<SoundEvent> m_soundEvents;
  /** Menus Back closed this frame, with their players (for CollectUISounds). */
  std::vector<SoundEvent> m_backSounds;
  /** The UI frame each player's menus last opened or closed in (0: the start): the highlight
   * that comes with it makes no Move sound. */
  std::array<uint64_t, LN_MAX_GAMEPADS> m_menuChangedFrame = {};
  std::unordered_map<int32_t, Highlight> m_highlights;
  std::vector<HighlightChange> m_highlightChanges;
  const LN_InputSnapshot *m_input = nullptr;
  LN_Event m_event = LN_Event::OnFixedUpdate;
  bool m_passive = false;
  mutable InteractionState m_passiveInteractionState;
  mutable ViewState m_passiveViewState;
  uint64_t m_tickIndex = UINT64_MAX;
  int64_t m_nextMenuHandle = 1;
  bool m_resolving = false;
  bool m_cancelConsumed = false;
  /** Players 1-7 whose held Back the UI already used (one bit each), as m_cancelConsumed is for
   * player 0: it closes or puts back one thing, and the game does not see it. */
  uint8_t m_playerBackUsed = 0;
  static const InteractionState s_emptyInteractionState;
  static const ViewState s_emptyViewState;
  static const std::string s_emptyContext;
  static uint64_t s_menuRevision;
  /** A menu with Pause Game is open in this scene. */
  bool m_menuPauseActive = false;
  /** Scenes with such a menu open, and whether their menus paused the game (it was running). */
  static int s_menuPauseScenes;
  static bool s_menuPausedGame;
};
