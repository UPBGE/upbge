# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>


import gpu
from gpu_extras.batch import batch_for_shader

from .widget import Widget, BGUI_DEFAULT


class ProgressBar(Widget):
  """A solid progress bar.
  Controlled via the 'percent' property which assumes percent as a 0-1 floating point number."""
  theme_section = 'ProgressBar'
  theme_options = {
        'FillColor1': (0.0, 0.42, 0.02, 1.0),
        'FillColor2': (0.0, 0.42, 0.02, 1.0),
        'FillColor3': (0.0, 0.42, 0.02, 1.0),
        'FillColor4': (0.0, 0.42, 0.02, 1.0),
        'BGColor1': (0, 0, 0, 1),
        'BGColor2': (0, 0, 0, 1),
        'BGColor3': (0, 0, 0, 1),
        'BGColor4': (0, 0, 0, 1),
        'BorderSize': 1,
        'BorderColor': (0, 0, 0, 1),
        }

  def __init__(self, parent, name=None, percent=1.0, sub_theme='', aspect=None, size=[1, 1], pos=[0, 0], options=BGUI_DEFAULT):
    """
    :param parent: the widget's parent
    :param name: the name of the widget
    :param percent: the initial percent
    :param sub_theme: sub type of theme to use
    :param aspect: constrain the widget size to a specified aspect ratio
    :param size: a tuple containing the width and height
    :param pos: a tuple containing the x and y position
    :param options: various other options
    """

    Widget.__init__(self, parent, name, aspect, size, pos, sub_theme, options)

    theme = self.theme

    self.fill_colors = [
        theme['FillColor1'],
        theme['FillColor2'],
        theme['FillColor3'],
        theme['FillColor4'],
        ]

    self.bg_colors = [
        theme['BGColor1'],
        theme['BGColor2'],
        theme['BGColor3'],
        theme['BGColor4'],
        ]

    self.border_color = theme['BorderColor']
    self.border = theme['BorderSize']

    self._percent = percent

    self.line_shader = gpu.shader.from_builtin('UNIFORM_COLOR')
    self.shader = gpu.shader.from_builtin('SMOOTH_COLOR')

  @property
  def percent(self):
    return self._percent

  @percent.setter
  def percent(self, value):
    self._percent = max(0.0, min(1.0, value))

  def _draw(self):
    """Draw the progress bar"""
    # Enable alpha blending
    gpu.state.blend_set('ALPHA_PREMULT')

    mid_x = self.gpu_view_position[0][0] + (self.gpu_view_position[1][0] - self.gpu_view_position[0][0]) * self._percent

    # Draw fill
    fill_vertices = (
    (self.gpu_view_position[0][0],self.gpu_view_position[0][1]),
    (mid_x, self.gpu_view_position[1][1]),
    (mid_x, self.gpu_view_position[2][1]),
    (self.gpu_view_position[3][0], self.gpu_view_position[3][1]))

    fill_indices  = ((0, 1, 3), (3, 1, 2))

    self.shader.bind()

    batch = batch_for_shader(self.shader, 'TRIS', {"pos": fill_vertices, "color":self.fill_colors}, indices=fill_indices)
    batch.draw(self.shader)

    # Draw bg
    bg_vertices = (
    (mid_x,self.gpu_view_position[0][1]),
    (self.gpu_view_position[1][0], self.gpu_view_position[1][1]),
    (self.gpu_view_position[2][0], self.gpu_view_position[2][1]),
    (mid_x, self.gpu_view_position[3][1]))

    bg_indices  = ((0, 1, 3), (3, 1, 2))

    self.shader.bind()

    batch = batch_for_shader(self.shader, 'TRIS', {"pos": bg_vertices, "color":self.bg_colors}, indices=bg_indices)
    batch.draw(self.shader)

    # Draw outline
    if self.border > 0:
      gpu.state.line_width_set(1 + self.border)
      self.line_shader.uniform_float("color", self.border_color)

      vertices = self.gpu_view_position
      lines = vertices[:] + [vertices[1], vertices[2], vertices[3], vertices[0]]
      batch = batch_for_shader(self.line_shader, 'LINES', {"pos": lines})
      batch.draw(self.line_shader)
      gpu.state.line_width_set(1.0)

    gpu.state.blend_set('NONE')

    Widget._draw(self)
