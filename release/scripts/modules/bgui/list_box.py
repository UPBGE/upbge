# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>

"""
ListBoxes make use of a ListBoxRenderer. The default ListBoxRenderer simply
displays an item's string representation. To make your own ListBoxRenderer
create a class that has a render_item() method that accepts the item to be rendered
and returns a widget to render.

ListBox Example
---------------

Here is an simple example of using the ListBox widget::

  class MySys(bgui.System):
    def lb_click(self, lb):
      print(lb.selected)

    def __init__(self):
      bgui.System.__init__(self)

      items = ["One", "Two", 4, 4.6]
      self.frame = bgui.Frame(self, 'window', border=2, size=[0.5, 0.5],
        options=bgui.BGUI_DEFAULT|bgui.BGUI_CENTERED)
      self.lb = bgui.ListBox(self.frame, "lb", items=items, padding=0.05, size=[0.9, 0.9], pos=[0.05, 0.05])
      self.lb.on_click = self.lb_click

      # ... rest of __init__

"""

from .widget import Widget, BGUI_DEFAULT, BGUI_MOUSE_CLICK
from .frame import Frame
from .label import Label


class ListBoxRenderer():
  """Base class for rendering an item in a ListBox"""
  def __init__(self, listbox):
    """
    :param listbox: the listbox the renderer will be used with (used for parenting)
    """
    self.label = Label(listbox, "label")

  def render_item(self, item):
    """Creates and returns a :py:class:`bgui.label.Label` representation of the supplied item

    :param item: the item to be rendered
    :rtype: :py:class:`bgui.label.Label`
    """
    self.label.text = str(item)

    return self.label


class ListBox(Widget):
  """Widget for displaying a list of data"""

  theme_section = 'ListBox'
  theme_options = {
        'HighlightColor1': (1, 1, 1, 1),
        'HighlightColor2': (0, 0, 1, 1),
        'HighlightColor3': (0, 0, 1, 1),
        'HighlightColor4': (0, 0, 1, 1),
        'Border': 1,
        'Padding': 0,
        }

  def __init__(self, parent, name=None, items=[], padding=0, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='', options=BGUI_DEFAULT):
    """
    :param parent: the widget's parent
    :param name: the name of the widget
    :param items: the items to fill the list with (can also be changed via ListBox.items)
    :param padding: the amount of extra spacing to put between items (can also be changed via ListBox.padding)
    :param aspect: constrain the widget size to a specified aspect ratio
    :param size: a tuple containing the width and height
    :param pos: a tuple containing the x and y position
    :param sub_theme: name of a sub_theme defined in the theme file (similar to CSS classes)
    :param options:  various other options
    """

    Widget.__init__(self, parent, name, aspect=aspect, size=size, pos=pos, sub_theme='', options=options)

    self._items = items
    #: The amount of extra spacing to put between items
    self.padding = padding if padding is not None else self.theme['Padding']

    self.highlight = Frame(self, "frame", border=1, size=[1, 1], pos=[0, 0])
    self.highlight.visible = False
    self.highlight.border = self.theme['Border']
    self.highlight.colors = [
        self.theme['HighlightColor1'],
        self.theme['HighlightColor2'],
        self.theme['HighlightColor3'],
        self.theme['HighlightColor4'],
        ]

    self.selected = None
    self._spatial_map = {}

    #: The ListBoxRenderer to use to display items
    self.renderer = ListBoxRenderer(self)

  @property
  def items(self):
    """The list of items to display in the ListBox"""
    return self._items

  @items.setter
  def items(self, value):
    self._items = value
    self._spatial_map.clear()

  def _draw(self):

    for idx, item in enumerate(self.items):
      w = self.renderer.render_item(item)
      w.position = [0, 1 - (idx + 1) * (w.size[1] / self.size[1]) - (idx * self.padding)]
      w.size = [1, w.size[1] / self.size[1]]
      self._spatial_map[item] = [i[:] for i in w.gpu_view_position]  # Make a full copy
      w._draw()

      if self.selected == item:
        self.highlight.gpu_view_position = [i[:] for i in w.gpu_view_position]
        self.highlight.visible = True

  def _handle_mouse(self, pos, event):

    if event == BGUI_MOUSE_CLICK:
      for item, gpu_view_position in self._spatial_map.items():
        if (gpu_view_position[0][0] <= pos[0] <= gpu_view_position[1][0]) and \
          (gpu_view_position[0][1] <= pos[1] <= gpu_view_position[2][1]):
            self.selected = item
            break
      else:
        self.selected = None

    Widget._handle_mouse(self, pos, event)
