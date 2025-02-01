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
An elementary json encoder-decoder to transmit Proxy and Delta items.

This module and the resulting encoding are by no way optimal. It is just a simple
implementation that does the job.
"""
from __future__ import annotations

import json
import logging
from typing import Dict, Tuple, TYPE_CHECKING, Union

if TYPE_CHECKING:
    from mixer.blender_data.proxy import Delta, Proxy

logger = logging.getLogger(__name__)

# https://stackoverflow.com/questions/38307068/make-a-dict-json-from-string-with-duplicate-keys-python/38307621#38307621
# https://stackoverflow.com/questions/31085153/easiest-way-to-serialize-object-in-a-nested-dictionary


MIXER_CLASS = "__mixer_class__"

_registry: Dict[str, Tuple[type, Tuple[str]]] = {}
"""Proxy class registry

{ class_name: (class, tuple of constructor arguments)}
"""


def serialize(_cls=None, *, ctor_args: Tuple[str, ...] = ()):
    """Class decorator to register a Proxy class for serialization.

    Args:
        - ctor_args: names of the attributes to pass to the constructor when deserializing
    """

    global _registry

    def wrapped(cls):
        try:
            _serialize = cls._serialize
            if not isinstance(_serialize, (tuple, list)):
                raise EncodeError(f"Expected tuple or list for _serialize, got {type(_serialize)} for {cls}")
        except AttributeError:
            cls._serialize = ()

        _registry[cls.__name__] = (cls, ctor_args)
        return cls

    if _cls is None:
        return wrapped
    else:
        return wrapped(_cls)


class EncodeError(Exception):
    pass


class DecodeError(Exception):
    pass


def default(obj):
    # called top down
    class_ = obj.__class__

    is_known = class_.__name__ in _registry
    if is_known:
        # Add the proxy class so that the decoder and instantiate the right type
        dict_ = {MIXER_CLASS: class_.__name__}
        for attribute_name in class_._serialize:
            attribute = getattr(obj, attribute_name, None)
            if attribute is not None:
                dict_.update({attribute_name: attribute})

        return dict_

    logger.error(f"Unregistered class {class_} for {obj}. Possible causes ...")
    logger.error(f"... no implemention for synchronization of {class_}")
    logger.error(f"... proxy class {class_} not loaded in mixer.blender_data.__init__.py")

    # returning None omits the data chunk from the serialization, but keeps the supported elements. An exception would
    # the whole update.
    return None


def decode_hook(x):
    class_name = x.get(MIXER_CLASS)
    class_, ctor_arg_names = _registry.get(class_name, (None, None))
    if class_ is None:
        return x

    del x[MIXER_CLASS]

    ctor_args = (x[name] for name in ctor_arg_names)
    obj = class_(*ctor_args)
    for attribute_name in class_._serialize:
        attribute = x.get(attribute_name, None)
        if attribute is not None:
            setattr(obj, attribute_name, attribute)

    return obj


class Codec:
    def encode(self, obj) -> str:
        return json.dumps(obj, default=default)

    def decode(self, message: str) -> Union[Proxy, Delta]:
        decoded = json.loads(message, object_hook=decode_hook)
        if isinstance(decoded, dict):
            raise DecodeError("decode failure", decoded)
        return decoded
