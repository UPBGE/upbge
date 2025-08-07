# SPDX-FileCopyrightText: 2024 Blender Authors
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


# These are substituted when this file is copied to the build directory.
BLENDER_VERSION_STRING = "${BLENDER_VERSION_STRING}"
BLENDER_VERSION_DOTS = "${BLENDER_VERSION_DOTS}"
BLENDER_REVISION = "${BLENDER_REVISION}"
BLENDER_REVISION_TIMESTAMP = "${BLENDER_REVISION_TIMESTAMP}"
BLENDER_VERSION_DATE = time.strftime(
    "%d/%m/%Y",
    time.localtime(int(BLENDER_REVISION_TIMESTAMP) if BLENDER_REVISION_TIMESTAMP != "0" else None),
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


project = "UPBGE 0.5x & Blender {:s} Python API".format(BLENDER_VERSION_STRING)
root_doc = "index"
copyright = "UPBGE & Blender Authors"
version = BLENDER_VERSION_DOTS
release = BLENDER_VERSION_DOTS

# Set this as the default is a super-set of Python3.
highlight_language = "python3"
# No need to detect encoding.
highlight_options = {"default": {"encoding": "utf-8"}}

# Quiet file not in table-of-contents warnings.
exclude_patterns = [
    "include__bmesh.rst",
]

html_title = "UPBGE/Blender Python API"

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
html_static_path = ["static"]

if html_theme == "sphinx_rtd_theme":
    html_css_files = ["css/theme_overrides_upbge.css"]

# The fallback to a built-in theme when `furo` is not found.
#html_theme = "default"

if has_module("furo"):
    html_theme = "furo"
    html_theme_options = {
        "light_css_variables": {
            "color-brand-primary": "#265787",
            "color-brand-content": "#265787",
        },
    }

    html_sidebars = {
        "**": [
            "sidebar/brand.html",
            "sidebar/search.html",
            "sidebar/scroll-start.html",
            "sidebar/navigation.html",
            "sidebar/scroll-end.html",
            "sidebar/variant-selector.html",
        ]
    }

# Not helpful since the source is generated, adds to upload size.
html_copy_source = False
html_show_sphinx = False
html_baseurl = "https://upbge.org/docs/latest/api"
html_use_opensearch = "https://upbge.org/docs/latest/api"
html_show_search_summary = True
html_split_index = True
html_static_path = ["static"]
templates_path = ["templates"]
html_context = {
    "commit": "{:s} - {:s}".format(BLENDER_VERSION_HASH_HTML_LINK, BLENDER_VERSION_DATE),
}
html_extra_path = ["static"]
html_favicon = "static/upbge_favicon.png"
html_logo = "static/upbge_logo.png"
# Disable default `last_updated` value, since this is the date of doc generation, not the one of the source commit.
html_last_updated_fmt = None
if html_theme == "furo":
    html_css_files = ["css/theme_overrides.css", "css/version_switch.css"]
    html_js_files = ["js/version_switch.js"]

# Needed for latex, PDF generation.
latex_elements = {
    "papersize": "a4paper",
}

latex_documents = [
    ("contents", "contents.tex", "Blender Index", "Blender Foundation", "manual"),
]

# Workaround for useless links leading to compile errors
# See https://github.com/sphinx-doc/sphinx/issues/3866
from sphinx.domains.python import PythonDomain


class PatchedPythonDomain(PythonDomain):
    def resolve_xref(self, env, fromdocname, builder, typ, target, node, contnode):
        if "refspecific" in node:
            del node["refspecific"]
        return super(PatchedPythonDomain, self).resolve_xref(
            env, fromdocname, builder, typ, target, node, contnode)


def setup(app):
    app.add_domain(PatchedPythonDomain, override=True)
