# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>


# Hack to make ReadTheDocs happy until they get Py3 support working again.
try:
  import configparser

  # The following is a bit of a hack so we can get our own SectionProxy in.
  # This allows us to return some nicer values from our config files
  configparser._SectionProxy = configparser.SectionProxy
except ImportError:
  import ConfigParser as configparser
  configparser._SectionProxy = object



class NewSectionProxy(configparser._SectionProxy):

  def __getitem__(self, key):
    val = configparser._SectionProxy.__getitem__(self, key)

    if isinstance(val, configparser._SectionProxy):
      return val

    # Lets try to make a nicer value
    try:
      return float(val)
    except ValueError:
      pass
    except TypeError:
      print(type(val))

    if ',' in val:
      try:
        val = [float(i) for i in val.split(',')]
      except ValueError:
        val = val.split(',')

      if isinstance(val[0], str) and val[0].startswith('img:'):
        val[0] = val[0].replace('img:', Theme.path)
        val[1:] = [float(i) for i in val[1:]]

    return val

configparser.SectionProxy = NewSectionProxy


class Theme(configparser.ConfigParser):
  path = ''

  def __init__(self, file):

    configparser.ConfigParser.__init__(self)

    if file:
      Theme.path = file + '/'
    else:
      Theme.path = './'

    if file:
      self.read(Theme.path + 'theme.cfg')

    self._legacy_warnings = []
    self._support_warnings = []

  def supports(self, widget):
    """Checks to see if the theme supports a given widget.

    :param widget: the widget to check for support
    """

    # First we see if we have the right section
    if not self.has_section(widget.theme_section):
      return False

    # Then we see if we have the required options
    for opt in widget.theme_options:
      if not self.has_option(widget.theme_section, opt):
        return False

    # All looks good, return True
    return True

  def warn_legacy(self, section):
    if section not in self._legacy_warnings:
      print("WARNING: Legacy theming used for", section)
      self._legacy_warnings.append(section)

  def warn_support(self, section):
    if section not in self._support_warnings:
      print("WARNING: Theming is enabled, but the current theme does not support", section)
      self._support_warnings.append(section)
