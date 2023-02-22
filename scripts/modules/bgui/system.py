# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>


import gpu

from .widget import Widget, BGUI_MOUSE_NONE, BGUI_NO_NORMALIZE, BGUI_NO_THEME
from .theme import Theme
import weakref


class System(Widget):
  """The main gui system. Add widgets to this and then call the render() method
  draw the gui.

  """

  normalize_text = True

  def __init__(self, theme=None):
    """
    :param theme: the path to a theme directory

    """

    # Size and positions for children to use.
    # The size will the the view port size and
    # the position will be the top left of the screen

    # Get some viewport info
    view = gpu.state.viewport_get()

    # Theming
    self._system = weakref.ref(self)
    self.theme = Theme(theme)

    Widget.__init__(self, self, "<System>", size=[view[2], view[3]],
          pos=[0, 0], options=BGUI_NO_NORMALIZE|BGUI_NO_THEME)

    self._focused_widget = weakref.ref(self)
    self.lock_focus = False

  @property
  def focused_widget(self):
    '''The widget which currently has "focus"'''
    return self._focused_widget()

  @focused_widget.setter
  def focused_widget(self, value):
    self._focused_widget = weakref.ref(value)

  def update_mouse(self, pos, click_state=BGUI_MOUSE_NONE):
    """Updates the system's mouse data

    :param pos: the mouse position
    :param click_state: the current state of the mouse
    :rtype: None

    """

    self.cursor_pos = pos

    Widget._handle_mouse(self, pos, click_state)

  def update_keyboard(self, key, is_shifted):
    """Updates the system's keyboard data

    :param key: the key being input
    :param is_shifted: is the shift key held down?
    :rtype: None
    """

    Widget._handle_key(self, key, is_shifted)

  def _attach_widget(self, widget):
    if widget == self:
      return

    Widget._attach_widget(self, widget)

  def render(self):
    """Renders the GUI system

    :rtype: None
    """

    # Get some viewport info
    view = gpu.state.viewport_get()

    # Update the size if the viewport has changed
    if self.size != [view[2], view[3]]:
      self.size = [view[2], view[3]]

    # Update any animations
    Widget._update_anims(self)

    # Render the windows
    Widget._draw(self)
