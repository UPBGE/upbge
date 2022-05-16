# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>


from .widget import Widget, BGUI_DEFAULT, BGUI_NO_THEME, BGUI_CENTERED
from .frame import Frame
from .label import Label

from copy import deepcopy


class FrameButton(Widget):
  """A clickable frame-based button."""
  theme_section = 'FrameButton'
  theme_options = {
        'Color1': (0.4, 0.4, 0.4, 1),
        'Color2': (0.4, 0.4, 0.4, 1),
        'Color3': (0.7, 0.7, 0.7, 1),
        'Color4': (0.7, 0.7, 0.7, 1),
        'BorderSize': 1,
        'BorderColor': (0, 0, 0, 1),
        'LabelSubTheme': '',
        }

  def __init__(self, parent, name=None, base_color=None, text="", font=None,
          pt_size=None, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='', options=BGUI_DEFAULT):
    """
    :param parent: the widget's parent
    :param name: the name of the widget
    :param base_color: the color of the button
    :param text: the text to display (this can be changed later via the text property)
    :param font: the font to use
    :param pt_size: the point size of the text to draw (defaults to 30 if None)
    :param aspect: constrain the widget size to a specified aspect ratio
    :param size: a tuple containing the width and height
    :param pos: a tuple containing the x and y position
    :param sub_theme: name of a sub_theme defined in the theme file (similar to CSS classes)
    :param options: various other options
    """

    Widget.__init__(self, parent, name, aspect, size, pos, sub_theme, options)

    self.frame = Frame(self, size=[1, 1], pos=[0, 0], options=BGUI_NO_THEME)
    self.label = Label(self, text=text, font=font, pt_size=pt_size, pos=[0, 0], sub_theme=self.theme['LabelSubTheme'], options=BGUI_DEFAULT | BGUI_CENTERED)

    self.frame.border = self.theme['BorderSize']
    self.frame.border_color = self.theme['BorderColor']

    self.base_colors = [
      list(self.theme["Color1"]), list(self.theme["Color2"]),
      list(self.theme["Color3"]), list(self.theme["Color4"])
    ]
    self.frame.colors = self.base_colors[:]

  @property
  def text(self):
    return self.label.text

  @text.setter
  def text(self, value):
    self.label.text = value
    
  @property
  def colors(self):
    return self.base_colors
    
  @colors.setter
  def colors(self, value):
    self.base_colors = value
    self.frame.colors = self.base_colors[:]

  def _handle_hover(self):
    cols = deepcopy(self.base_colors)

    # Lighten button when hovered over.
    for col in cols:
      for n in range(3):
        col[n] += .1
    self.frame.colors = cols

  def _handle_active(self):
    cols = deepcopy(self.base_colors)

    # Lighten button when hovered over.
    for col in cols:
      for n in range(3):
        col[n] -= .1
    self.frame.colors = cols

  def _draw(self):
    """Draw the button"""

    # Draw the children before drawing an additional outline
    Widget._draw(self)

    # Reset the button's color
    self.frame.colors = self.base_colors[:]
