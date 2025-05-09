#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

__all__ = (
    "main",
)

import os
from os.path import join

from collections.abc import (
    Callable,
    Iterator,
    Sequence,
)

from trailing_space_clean_config import PATHS

SOURCE_EXT = (
    # C/C++
    ".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".cxx", ".hxx", ".inl",
    # Objective C
    ".m", ".mm",
    # GLSL
    ".glsl",
    # Python
    ".py",
    # TOML.
    ".toml",
    # Text (also CMake)
    ".txt", ".cmake", ".rst",
    # MS-Windows Scripts.
    ".bat", ".cmd",
    # HTML, XML.
    ".html",
    ".xml",
)


def is_source(filename: str) -> bool:
    return filename.endswith(SOURCE_EXT)


def path_iter(
        path: str,
        filename_check: Callable[[str], bool] | None = None,
) -> Iterator[str]:
    for dirpath, dirnames, filenames in os.walk(path):
        # skip ".git"
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]

        for filename in filenames:
            if filename.startswith("."):
                continue
            filepath = join(dirpath, filename)
            if filename_check is None or filename_check(filepath):
                yield filepath


def path_expand(
        paths: Sequence[str],
        filename_check: Callable[[str], bool] | None = None,
) -> Iterator[str]:
    for f in paths:
        if not os.path.exists(f):
            print("Missing:", f)
        elif os.path.isdir(f):
            yield from path_iter(f, filename_check)
        else:
            yield f


def rstrip_file(filename: str) -> tuple[str, ...]:
    reports = []
    with open(filename, "r", encoding="utf-8") as fh:
        data_src = fh.read()

    # Strip trailing space.
    data_dst_list = []
    for l in data_src.rstrip().splitlines(True):
        data_dst_list.append(l.rstrip() + "\n")

    data_dst = "".join(data_dst_list)
    del data_dst_list

    # Remove BOM.
    if data_dst and (data_dst[0] == '\ufeff'):
        data_dst = data_dst[1:]

    len_strip = len(data_src) - len(data_dst)
    if len_strip != 0:
        reports.append("STRIP={:d}".format(len_strip))

    if len_strip:
        with open(filename, "w", encoding="utf-8") as fh:
            fh.write(data_dst)
    return tuple(reports)


def main() -> None:
    for f in path_expand(PATHS, is_source):
        report = rstrip_file(f)
        if report:
            print("Strip ({:s}): {:s}".format(', '.join(report), f))


if __name__ == "__main__":
    main()
