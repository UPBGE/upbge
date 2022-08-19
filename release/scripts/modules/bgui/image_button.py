# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>


from .widget import Widget, BGUI_DEFAULT
from .image import Image
from .frame import Frame


class ImageButton(Widget):
  """A clickable image-based button."""

  theme_section = 'ImageButton'
  theme_options = {
        'DefaultImage': (None, 0, 0, 1, 1),
        'Default2Image': (None, 0, 0, 1, 1),
        'HoverImage': (None, 0, 0, 1, 1),
        'ClickImage': (None, 0, 0, 1, 1),
        }

  def __init__(self, parent, name=None, default_image=None, default2_image=None, hover_image=None,
          click_image=None, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='',
          options=BGUI_DEFAULT):
    """
    :param parent: the widget's parent
    :param name: the name of the widget
    :param default_image: list containing image data for the default state ('image', xcoord, ycoord, xsize, ysize)
    :param default2_image: list containing image data for a second default state, which is used for toggling ('image', xcoord, ycoord, xsize, ysize)
    :param hover_image: list containing image data for the hover state ('image', xcoord, ycoord, xsize, ysize)
    :param click_image: list containing image data for the click state ('image', xcoord, ycoord, xsize, ysize)
    :param aspect: constrain the widget size to a specified aspect ratio
    :param size: a tuple containing the width and height
    :param pos: a tuple containing the x and y position
    :param sub_theme: name of a sub_theme defined in the theme file (similar to CSS classes)
    :param options: various other options
    """

    Widget.__init__(self, parent, name, aspect, size, pos, sub_theme, options)

    if default_image:
      self.default_image = default_image
    else:
      self.default_image = self.theme['DefaultImage']

    if default2_image:
      self.default2_image = default2_image
    else:
      self.default2_image = self.theme['Default2Image']

    if hover_image:
      self.hover_image = hover_image
    else:
      self.hover_image = self.theme['HoverImage']

    if click_image:
      self.click_image = click_image
    else:
      self.click_image = self.theme['ClickImage']

    if self.default_image[0]:
      coords = self._get_coords(self.default_image)
      self.image = Image(self, self.default_image[0],
                texco=coords, size=[1, 1], pos=[0, 0])
    else:
      self.image = Frame(self, size=[1, 1], pos=[0, 0])

    self.state = 0

  def _get_coords(self, image):
    v = image[1:]
    return [(v[0], v[1]), (v[0] + v[2], v[1]), (v[0] + v[2], v[1] + v[3]), (v[0], v[1] + v[3])]

  def _update_image(self, image):
    if image[0]:
      self.image.texco = self._get_coords(image)
      self.image.update_image(image[0])

  def _get_default_image(self):
    if self.state == 1 and self.default_image[0]:
      return self.default2_image

    return self.default_image

  def _handle_click(self):
    self.state = not self.state

  def _handle_release(self):
    self._update_image(self._get_default_image())

  def _handle_active(self):
    self._update_image(self.click_image)

  def _handle_hover(self):
    self._update_image(self.hover_image)

  def _handle_mouse_exit(self):
    self._update_image(self._get_default_image())

  def _draw(self):
    Widget._draw(self)
