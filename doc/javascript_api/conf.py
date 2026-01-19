# SPDX-FileCopyrightText: 2024 UPBGE Contributors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import time


def has_module(module_name):
    found = False
    try:
        __import__(module_name)
        found = True
    except ModuleNotFoundError as ex:
        if ex.name != module_name:
            raise ex
    return found


# For JavaScript API, we use fixed version info (can be overridden via environment)
# These would be substituted if integrated with build system
BLENDER_VERSION_STRING = os.environ.get("BLENDER_VERSION_STRING", "0.5")
BLENDER_VERSION_DOTS = os.environ.get("BLENDER_VERSION_DOTS", "0.5")
BLENDER_REVISION = os.environ.get("BLENDER_REVISION", "Unknown")
BLENDER_REVISION_TIMESTAMP = os.environ.get("BLENDER_REVISION_TIMESTAMP", "0")
BLENDER_VERSION_DATE = time.strftime(
    "%d/%m/%Y",
    time.localtime(int(BLENDER_REVISION_TIMESTAMP) if BLENDER_REVISION_TIMESTAMP != "0" else time.time()),
)

if BLENDER_REVISION != "Unknown":
    # SHA1 GIT hash.
    BLENDER_VERSION_HASH = BLENDER_REVISION
    BLENDER_VERSION_HASH_HTML_LINK = (
        "<a href=https://projects.blender.org/blender/blender/commit/{:s}>{:s}</a>".format(
            BLENDER_VERSION_HASH, BLENDER_VERSION_HASH,
        )
    )
else:
    # Fallback: Should not be used.
    BLENDER_VERSION_HASH = "Hash Unknown"
    BLENDER_VERSION_HASH_HTML_LINK = BLENDER_VERSION_HASH

extensions = []

# Downloading can be slow and get in the way of development,
# support "offline" builds.
if not os.environ.get("BLENDER_DOC_OFFLINE", "").strip("0"):
    extensions.append("sphinx.ext.intersphinx")
    intersphinx_mapping = {"blender_manual": ("https://docs.blender.org/manual/en/dev/", None)}

# Provides copy button next to code-blocks (nice to have but not essential).
if has_module("sphinx_copybutton"):
    extensions.append("sphinx_copybutton")

    # Exclude line numbers, prompts, and console text.
    copybutton_exclude = ".linenos, .gp, .go"


project = "UPBGE 0.5x JavaScript/TypeScript API"
root_doc = "index"
copyright = "UPBGE Contributors"
# For JavaScript API, we can use fixed version or get from environment
# version = BLENDER_VERSION_DOTS
# release = BLENDER_VERSION_DOTS
version = "0.5"
release = "0.5"

# Set this as the default for JavaScript/TypeScript
highlight_language = "javascript"
# No need to detect encoding.
highlight_options = {"default": {"encoding": "utf-8"}}

# Quiet file not in table-of-contents warnings.
exclude_patterns = []

html_title = "UPBGE JavaScript/TypeScript API"

# -- Options for UPBGE HTML output -------------------------------------------------

# Add any paths that contain custom themes here, relative to this directory.
# The theme to use for HTML and HTML Help pages.  See the documentation for
# a list of builtin themes.
#
import sphinx_rtd_theme

html_theme = 'sphinx_rtd_theme'

# Theme options
html_theme_options = {
        # included in the title
        "display_version": False,
        "collapse_navigation": True,
        "navigation_depth": -1,
        "body_max_width": "80%",
    }

extensions.append('sphinx_rtd_theme')

# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = ['static']

# Custom sidebar templates, must be a dictionary that maps document names
# to template names.
html_sidebars = {
    '**': [
        'sidebar/variant-selector.html',
    ],
}

# If true, links to the reST sources are added to the pages.
html_show_sourcelink = False

# If true, "Created using Sphinx" is shown in the HTML footer. Default is True.
html_show_sphinx = False

# Output file base name for HTML help builder.
htmlhelp_basename = 'UPBGEJavaScriptAPI'
