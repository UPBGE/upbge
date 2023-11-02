# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>

from .widget import Widget, BGUI_DEFAULT

import gpu
from gpu_extras.batch import batch_for_shader


class Frame(Widget):
  """Frame for storing other widgets"""
  theme_section = 'Frame'
  theme_options = {
        'Color1': (0, 0, 0, 0),
        'Color2': (0, 0, 0, 0),
        'Color3': (0, 0, 0, 0),
        'Color4': (0, 0, 0, 0),
        'BorderSize': 0,
        'BorderColor': (0, 0, 0, 1),
        }

  def __init__(self, parent, name=None, border=None, aspect=None, size=[1, 1], pos=[0, 0],
               sub_theme='', options=BGUI_DEFAULT):
    """
    :param parent: the widget's parent
    :param name: the name of the widget
    :param border: the size of the border around the frame (0 for no border)
    :param aspect: constrain the widget size to a specified aspect ratio
    :param size: a tuple containing the width and height
    :param pos: a tuple containing the x and y position
    :param sub_theme: name of a sub_theme defined in the theme file (similar to CSS classes)
    :param options: various other options
    """

    Widget.__init__(self, parent, name, aspect, size, pos, sub_theme, options)

    #: The colors for the four corners of the frame.
    self.colors = [
        self.theme['Color1'],
        self.theme['Color2'],
        self.theme['Color3'],
        self.theme['Color4']
        ]

    #: The color of the border around the frame.
    self.border_color = self.theme['BorderColor']
    
    #: The size of the border around the frame.
    if border is not None:
      self.border = border
    else:
      self.border = self.theme['BorderSize']

    self.line_shader = gpu.shader.from_builtin('UNIFORM_COLOR')
    self.shader = gpu.shader.from_builtin('SMOOTH_COLOR')

  def _draw(self):
    """Draw the frame"""

    gpu.state.blend_set('ALPHA_PREMULT')

    colors = self.colors
    vertices = self.gpu_view_position
    indices  = ((0, 1, 3), (3, 1, 2))

    self.shader.bind()

    batch = batch_for_shader(self.shader, 'TRIS', {"pos": vertices, "color":colors}, indices=indices)
    batch.draw(self.shader)

    if self.border > 0:
      gpu.state.line_width_set(1 + self.border)
      self.line_shader.uniform_float("color", self.border_color)

      lines = vertices[:] + [vertices[1], vertices[2], vertices[3], vertices[0]]
      batch = batch_for_shader(self.line_shader, 'LINES', {"pos": lines})
      batch.draw(self.line_shader)
      gpu.state.line_width_set(1.0)

    gpu.state.blend_set('NONE')

    Widget._draw(self)
