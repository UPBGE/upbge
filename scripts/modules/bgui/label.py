# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>

from .widget import Widget, BGUI_DEFAULT, BGUI_NO_NORMALIZE

import blf


class Label(Widget):
  """Widget for displaying text"""
  theme_section = 'Label'
  theme_options = {
        'Font': '',
        'Color': (1, 1, 1, 1),
        'OutlineColor': (0, 0, 0, 1),
        'OutlineSize': 0,
        'OutlineSmoothing': False,
        'Size': 30,
        }

  def __init__(self, parent, name=None, text="", font=None, pt_size=None, color=None,
        outline_color=None, outline_size=None, outline_smoothing=None, pos=[0, 0], sub_theme='', options=BGUI_DEFAULT):
    """
    :param parent: the widget's parent
    :param name: the name of the widget
    :param text: the text to display (this can be changed later via the text property)
    :param font: the font to use
    :param pt_size: the point size of the text to draw (defaults to 30 if None)
    :param color: the color to use when rendering the font
    :param pos: a tuple containing the x and y position
    :param sub_theme: name of a sub_theme defined in the theme file (similar to CSS classes)
    :param options: various other options

    """
    Widget.__init__(self, parent, name, None, [0, 0], pos, sub_theme, options)

    if font:
      self.fontid = blf.load(font)
    else:
      font = self.theme['Font']
      self.fontid = blf.load(font) if font else 0

    if pt_size:
      self.pt_size = pt_size
    else:
      self.pt_size = self.theme['Size']

    if color:
      self.color = color
    else:
      self.color = self.theme['Color']

    if outline_color:
      self.outline_color = outline_color
    else:
      self.outline_color = self.theme['OutlineColor']

    if outline_size is not None:
      self.outline_size = outline_size
    else:
      self.outline_size = self.theme['OutlineSize']
    self.outline_size = int(self.outline_size)

    if outline_smoothing is not None:
      self.outline_smoothing = outline_smoothing
    else:
      self.outline_smoothing = self.theme['OutlineSmoothing']

    self.text = text

  @property
  def text(self):
    """The text to display"""
    return self._text

  @text.setter
  def text(self, value):
    blf.size(self.fontid, self.pt_size)
    size = [blf.dimensions(self.fontid, value)[0], blf.dimensions(self.fontid, 'Mj')[0]]

    if not (self.options & BGUI_NO_NORMALIZE):
      size[0] /= self.parent.size[0]
      size[1] /= self.parent.size[1]

    self._update_position(size, self._base_pos)

    self._text = value

  @property
  def pt_size(self):
    """The point size of the label's font"""
    return self._pt_size

  @pt_size.setter
  def pt_size(self, value):
    # Normalize the pt size (1000px height = 1)
    if self.system.normalize_text:
      self._pt_size = int(value * (self.system.size[1] / 1000))
    else:
      self._pt_size = value

  def _draw_text(self, x, y):
    for i, txt in enumerate([i for i in self._text.split('\n')]):
      blf.position(self.fontid, x, y - (self.size[1] * i), 0)
      blf.draw(self.fontid, txt.replace('\t', '    '))

  def _draw(self):
    """Display the text"""

    blf.size(self.fontid, self.pt_size)

    blf.color(self.fontid, *self.color)
    self._draw_text(*self.position)

    Widget._draw(self)
