# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>

from .system import System as BguiSystem
from .widget import Widget, BGUI_MOUSE_NONE, BGUI_MOUSE_CLICK, BGUI_MOUSE_RELEASE, BGUI_MOUSE_ACTIVE
from . import key_defs
from bge import logic, events, render
import collections


class Layout(Widget):
  """A base layout class to be used with the BGESystem"""

  def __init__(self, sys, data):
    """
    :param sys: The BGUI system
    :param data: User data
    """

    super().__init__(sys, size=[1,1])
    self.data = data

  def update(self):
    """A function that is called by the system to update the widget (subclasses should override this)"""
    pass


class System(BguiSystem):
  """A system that is intended to be used with BGE games"""

  def __init__(self, theme=None):
    """
    :param theme: the path to a theme directory

    """
    super().__init__(theme)

    self.mouse = logic.mouse

    # All layouts will be a widget subclass, so we can just keep track of one widget
    self.layout = None

    # We can also add 'overlay' layouts
    self.overlays = collections.OrderedDict()

    # Now we generate a dict to map BGE keys to bgui keys
    self.keymap = {getattr(events, val): getattr(key_defs, val) for val in dir(events) if val.endswith('KEY') or val.startswith('PAD')}

    # Now setup the scene callback so we can draw
    logic.getCurrentScene().post_draw.append(self._render)

  def load_layout(self, layout, data=None):
    """Load a layout and replace any previously loaded layout

    :param layout: The layout to load (None to have no layouts loaded)
    :param data: User data to send to the layout's constructor
    """

    if self.layout:
      self._remove_widget(self.layout)

    if layout:
      self.layout = layout(self, data)
    else:
      self.layout = None

  def add_overlay(self, overlay, data=None):
    """Add an overlay layout, which sits on top of the currently loaded layout

    :param overlay: The layout to add as an overlay
    :param data: User data to send to the layout's constructor"""

    name = overlay.__class__.__name__

    if name in self.overlays:
      print("Overlay: %s, is already added" % name)
      return

    self.overlays[overlay.__class__.__name__] = overlay(self, data)

  def remove_overlay(self, overlay):
    """Remove an overlay layout by name

    :param overlay: the class name of the overlay to remove (this is the same name as the layout used to add the overlay)
    """

    name = overlay.__class__.__name__

    if name in self.overlays:
      self._remove_widget(self.overlays[name])
      del self.overlays[name]
    else:
      print("WARNING: Overlay: %s was not found, nothing was removed" % name)

  def toggle_overlay(self, overlay, data=None):
    """Toggle an overlay (if the overlay is active, remove it, otherwise add it)

    :param overlay: The class name of the layout to toggle
    :param data: User data to send to the layout's constructor
    """

    if overlay.__class__.__name__ in self.overlays:
      self.remove_overlay(overlay)
    else:
      self.add_overlay(overlay, data)

  def _render(self):
    try:
      super().render()
    except:
      # If there was a problem with rendering, stop so we don't spam the console
      import traceback
      traceback.print_exc()
      logic.getCurrentScene().post_draw.remove(self._render)

  def run(self):
    """A high-level method to be run every frame"""

    if not self.layout:
      return

    # Update the layout and overlays
    self.layout.update()

    for key, value in self.overlays.items():
      value.update()

    # Handle the mouse
    mouse = self.mouse
    mouse_events = mouse.inputs

    pos = list(mouse.position[:])
    pos[0] *= render.getWindowWidth()
    pos[1] = render.getWindowHeight() - (render.getWindowHeight() * pos[1])

    if mouse_events[events.LEFTMOUSE].activated:
      mouse_state = BGUI_MOUSE_CLICK
    elif mouse_events[events.LEFTMOUSE].released:
      mouse_state = BGUI_MOUSE_RELEASE
    elif mouse_events[events.LEFTMOUSE].active:
      mouse_state = BGUI_MOUSE_ACTIVE
    else:
      mouse_state = BGUI_MOUSE_NONE

    self.update_mouse(pos, mouse_state)

    # Handle the keyboard
    keyboard = logic.keyboard

    key_events = keyboard.inputs
    is_shifted = key_events[events.LEFTSHIFTKEY].active or \
                 key_events[events.RIGHTSHIFTKEY].active

    for key, state in key_events.items():
      if state.activated:
        self.update_keyboard(self.keymap[key], is_shifted)
