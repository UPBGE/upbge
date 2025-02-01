# MIT License
#
# Copyright (c) 2020 Ubisoft
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""
Utility functions for CLI applications, mostly related to logging and argument parsing.
"""

import logging
import logging.handlers
import argparse


def init_logging(args):
    """
    Initialize root logger of an application.
    """
    log_numeric_level = getattr(logging, args.log_level.upper(), None)
    if not isinstance(log_numeric_level, int):
        raise ValueError(f"Invalid log level: {args.log_level}")

    formatter = logging.Formatter(fmt="%(asctime)s - %(name)s - %(levelname)s - %(message)s [%(pathname)s:%(lineno)d]")
    handler = logging.StreamHandler()
    handler.setFormatter(formatter)

    logger = logging.getLogger()

    logger.addHandler(handler)
    logger.setLevel(log_numeric_level)

    if args.log_file:
        max_bytes = 1024 * 1000 * 512  # 512 MB
        backup_count = 1024 * 1000  # 1B backup files
        handler = logging.handlers.RotatingFileHandler(args.log_file, maxBytes=max_bytes, backupCount=backup_count)
        handler.setFormatter(formatter)
        logger.addHandler(handler)

        logger.info(f"Logging to file {args.log_file}")


def add_logging_cli_args(parser: argparse.ArgumentParser):
    """
    Set CLI arguments for logger configuration.
    """
    parser.add_argument("--log-level", default="WARNING", help="Level of log to use by default.")
    parser.add_argument("--log-file", help="Path to log file.")
