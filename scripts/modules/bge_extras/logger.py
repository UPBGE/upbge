# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8-80 compliant>
import bpy
import json
import logging

from pathlib import Path
from logging import Handler, NOTSET, INFO, DEBUG, config


def setup(log_level: int) -> None:
    if log_level == NOTSET:
        logging.disable()
        return

    logging.disable(NOTSET)

    config_file = Path(bpy.path.abspath("//logging.json"))

    log_conf = None

    if config_file.exists():
        with open(config_file) as f:
            print(f"Loading logging configuration from {config_file}.")

            log_conf = json.load(f)
    else:
        log_conf = {
            "version": 1,
            "disable_existing_loggers": False,
            "loggers": {
                "root": {
                    "handlers": [
                        "stdout",
                        "console"
                    ]
                }
            },
            "formatters": {
                "simple": {
                    "format": "[%(levelname)s] %(name)s - %(message)s"
                }
            },
            "handlers": {
                "stdout": {
                    "class": "logging.StreamHandler",
                    "formatter": "simple",
                    "stream": "ext://sys.stdout"
                },
                "console": {
                    "class": "bge_extras.logger.ConsoleLogger",
                    "formatter": "simple"
                }
            }
        }

    if log_conf["loggers"]:
        root = log_conf["loggers"]["root"]

        if root:
            root["level"] = logging.getLevelName(log_level)

    logging.config.dictConfig(log_conf)


class ConsoleLogger(Handler):
    def __init__(self, level=NOTSET) -> None:
        super().__init__(level)

    def emit(self, record) -> None:
        context_override = None

        if not hasattr(bpy.context.screen, "areas"):
            return

        for area in bpy.context.screen.areas:
            if area.type == "CONSOLE":
                context_override = {
                    "area": area,
                    "space_data": area.spaces.active,
                    "region": area.regions[-1],
                    "window": bpy.context.window,
                    "screen": bpy.context.screen
                }

        if not context_override:
            return

        # noinspection PyBroadException
        try:
            msg = self.format(record)

            for line in msg.split("\n"):
                # noinspection PyArgumentList
                msg_type: str

                if record.levelno > INFO:
                    msg_type = "ERROR"
                elif record.levelno > DEBUG:
                    msg_type = "INFO"
                else:
                    msg_type = "OUTPUT"

                # noinspection PyArgumentList
                with bpy.context.temp_override(**context_override):
                    bpy.ops.console.scrollback_append(text=line, type=msg_type)

            context_override["area"].tag_redraw()

            self.flush()
        except RecursionError:
            raise
        except Exception:
            self.handleError(record)
