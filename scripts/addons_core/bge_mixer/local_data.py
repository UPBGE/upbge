# GPLv3 License
#
# Copyright (C) 2020 Ubisoft
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""
This module defines utilities for local data.
"""

import os
import logging
import tempfile
from pathlib import Path
import hashlib
import bpy

logger = logging.getLogger(__name__)


def get_data_directory():
    if "MIXER_DATA_DIR" in os.environ:
        data_path = Path(os.environ["MIXER_DATA_DIR"])
        if os.path.exists(data_path):
            return data_path
        logger.error(
            f"MIXER_DATA_DIR env var set to {data_path}, but directory does not exists. Falling back to default location."
        )
    return str(Path(os.fspath(tempfile.gettempdir())) / "mixer" / "data")


def get_resolved_file_path(path: str):
    abspath = bpy.path.abspath(path)
    if os.path.exists(abspath):
        return path

    return str(get_cache_file_hash(Path(path)))


def get_cache_file_path(path: str):
    return str(get_cache_file_hash(Path(path)))


def get_source_file_path(cache_path: str):
    if cache_path is None:
        return None
    metadata_path = Path(cache_path).with_suffix(".metadata")
    try:
        if not metadata_path.exists():
            return cache_path
    except Exception:
        logger.warning("Error accessing file " + cache_path)
        return None
    return metadata_path.read_text()


def get_local_or_create_cache_file(str_path: str, data: bytes):
    path = Path(str_path)
    if path.exists():
        return str_path

    cache_path = get_cache_file_hash(path)
    if not cache_path.with_suffix(".metadata").exists():
        create_cache_file(path, cache_path, data)
    return str(cache_path)


def get_or_create_cache_file(str_path: str, data: bytes):
    path = Path(str_path)
    cache_path = get_cache_file_hash(path)

    if not cache_path.with_suffix(".metadata").exists():
        create_cache_file(path, cache_path, data)
    return str(cache_path)


def get_cache_file_hash(path: Path):
    m = hashlib.sha1()
    m.update(str(path).encode())
    return Path(get_data_directory()) / "images" / str(m.hexdigest() + path.suffix)


def create_cache_file(path: Path, cache_path: Path, data: bytes):
    cache_path.parent.mkdir(parents=True, exist_ok=True)

    with open(cache_path, "wb") as data_file:
        data_file.write(data)

    with open(cache_path.with_suffix(".metadata"), "w") as metadata_file:
        metadata_file.write(str(path))
