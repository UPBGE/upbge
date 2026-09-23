"""
BGUI UI Component — Template for UPBGE
======================================

A ready-to-use BGUI (Basic GUI) component that provides:
  - A simple HUD overlay (top bar + title label + pause button)
  - A full-screen Pause menu with Resume button

No external assets required. Works out of the box with default font/colors.
Optionally accepts a theme path and font file if packed in your .blend.

IMPORTANT NOTES:
  - bgui imports are done inside start() because bgui.bgui_utils imports
    bge.logic which only exists at BGE runtime (not in Blender editor).
  - Widget tree modifications (add/remove overlay) MUST be deferred outside
    of _handle_mouse to avoid "OrderedDict mutated during iteration" crashes.
  - The system uses bge.render.getWindowWidth()/Height() for viewport size,
    which is valid from the start of player mode (unlike gpu.state.viewport_get()).

USAGE:
  1. Add this component to any object in your scene (e.g., a Camera or an empty).
  2. Optionally pack a theme.cfg and font file into your .blend and set the paths.
  3. Run in Game Engine mode — the HUD will appear automatically.

AUTHOR: Qwen3.8
LICENSE: MIT
"""

import bge
from collections import OrderedDict


class %Name%(bge.types.KX_PythonComponent):
    """BGUI UI component — HUD with Pause overlay.

    Works without any external assets (theme, font, image).
    If you want custom theming, pack a theme.cfg and set "Theme Path".
    """

    args = OrderedDict([
        # Optional: path to a BGUI theme directory packed in the .blend
        # (contains theme.cfg). Leave empty for default colors.
        ("Theme Path", ""),
        # Optional: path to a font file packed in the .blend (.otf, .ttf, .woff2)
        # Leave empty to use Blender's built-in default font.
        ("Font File", ""),
        # HUD title text displayed in the top bar
        ("HUD Title", "My Game"),
    ])

    def start(self, args):
        """Initialize BGUI system and load the HUD layout."""
        import bgui
        import bgui.bgui_utils

        self.scene = bge.logic.getCurrentScene()

        # --- Resolve optional paths ---
        theme_path = None
        if args["Theme Path"]:
            theme_path = bge.logic.expandPath(args["Theme Path"])

        font_path = None
        if args["Font File"]:
            font_path = bge.logic.expandPath(args["Font File"])

        # Load font once (fontid=0 is Blender's default font, always valid)
        import blf
        self.font_id = blf.load(font_path) if font_path else 0

        hud_title = args["HUD Title"]

        # --- Define Layouts (local classes, bgui already imported) ---

        class GameHUD(bgui.bgui_utils.Layout):
            """Main HUD: top bar with title + pause button."""

            def __init__(self, sys, data):
                super().__init__(sys, data)

                self.font_id = data.get("font_id") if data else 0
                font = self.font_id

                # Top bar (semi-transparent dark strip)
                self.top_bar = bgui.Frame(self, size=[1, 0.08], pos=[0, 0.92])
                self.top_bar.colors = [(0.1, 0.1, 0.15, 0.6)] * 4

                # Title label
                self.title = bgui.Label(
                    self.top_bar, text=hud_title,
                    font=font, pt_size=30, color=(1, 1, 1, 1),
                    pos=[0.02, 0.5], options=bgui.BGUI_CENTERY
                )

                # Pause button (bottom-right)
                self.pause_btn = bgui.FrameButton(
                    self, text="Pause", font=font, pt_size=24,
                    size=[0.1, 0.05], pos=[0.89, 0.03]
                )
                self.pause_btn.on_click = self._on_pause

            def update(self):
                pass

            def _on_pause(self, widget):
                # DO NOT modify the widget tree here!
                # Widget._handle_mouse iterates self.children (OrderedDict).
                # Modifying it during iteration causes RuntimeError.
                # Solution: set a flag, process in update_deferred().
                self._pending_toggle = True

            def update_deferred(self):
                """Call after sys.run() to safely toggle overlays."""
                if getattr(self, '_pending_toggle', False):
                    self._pending_toggle = False
                    self.system.toggle_overlay(
                        PauseMenu, {"font_id": self.font_id}
                    )

        class PauseMenu(bgui.bgui_utils.Layout):
            """Full-screen pause overlay with Resume button."""

            def __init__(self, sys, data):
                super().__init__(sys, data)

                font = data.get("font_id") if data else None

                # Dark background
                self.bg = bgui.Frame(self, size=[1, 1])
                self.bg.colors = [(0, 0, 0, 0.7)] * 4

                # "PAUSE" title
                self.title = bgui.Label(
                    self, text="PAUSE", font=font, pt_size=60,
                    pos=[0.5, 0.7], options=bgui.BGUI_CENTERX
                )

                # Resume button (centered)
                self.resume_btn = bgui.FrameButton(
                    self, text="Resume", font=font, pt_size=32,
                    size=[0.3, 0.08], pos=[0.5, 0.4],
                    options=bgui.BGUI_CENTERX | bgui.BGUI_CENTERY
                )
                self.resume_btn.on_click = self._on_resume

            def _on_resume(self, widget):
                # Same deferred pattern — don't remove overlay during mouse handling
                self._pending_remove = True

            def update_deferred(self):
                if getattr(self, '_pending_remove', False):
                    self._pending_remove = False
                    self.system.remove_overlay(PauseMenu)

        # --- Initialize BGUI system ---
        self.sys = bgui.bgui_utils.System(theme_path)
        self.sys.load_layout(GameHUD, data={"font_id": self.font_id})

        # Show mouse cursor (hidden by default in BGE)
        bge.logic.mouse.visible = True

    def update(self):
        """Per-frame: run BGUI system and process deferred actions."""
        self.sys.run()

        # Process deferred widget-tree modifications (safe, outside _handle_mouse)
        layout = self.sys.layout
        if layout and hasattr(layout, 'update_deferred'):
            layout.update_deferred()
        for key, overlay in self.sys.overlays.items():
            if hasattr(overlay, 'update_deferred'):
                overlay.update_deferred()

    def dispose(self):
        """Cleanup: hide mouse cursor when component is removed."""
        bge.logic.mouse.visible = False
