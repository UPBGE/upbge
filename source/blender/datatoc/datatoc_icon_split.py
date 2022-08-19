# SPDX-License-Identifier: GPL-2.0-or-later

"""
This script dices up PNG into small files to store in version control.

Example:

./blender.bin \
    --background -noaudio \
    --python ./release/datafiles/icon_dice.py -- \
    --image=./release/datafiles/blender_icons16.png \
    --output=./release/datafiles/blender_icons16
    --output_prefix=icon16_
    --name_style=UI_ICONS
    --parts_x 26 --parts_y 32 \
    --minx=10 --maxx 10 --miny 10 --maxy 10
    --minx_icon 2 --maxx_icon 2 --miny_icon 2 --maxy_icon 2 \
    --spacex_icon 1 --spacey_icon 1

"""

import os

SOURCE_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
VERBOSE = False


def image_from_file__bpy(filepath):
    import bpy

    image = bpy.data.images.load(filepath)
    image.reload()

    pixel_w, pixel_h = image.size
    pixels = image.pixels[:]
    return pixels, pixel_w, pixel_h


def image_from_file(filepath):
    """
    Return pixels, w, h from an image.

    note: bpy import is ONLY used here.
    """

    try:
        import bpy
    except ImportError:
        bpy = None

    if bpy is not None:
        pixels, pixel_w, pixel_h = image_from_file__bpy(filepath)
    # else:
    #    pixels, pixel_w, pixel_h = image_from_file__py(filepath)

    return pixels, pixel_w, pixel_h


def write_subimage(sub_x, sub_y, sub_w, sub_h,
                   filepath,
                   pixels, pixel_w, pixel_h):
    import struct

    # first check if the icon is worth writing
    is_fill = False
    for y in range(sub_h):
        for x in range(sub_w):
            i = (sub_x + x) + ((sub_y + y) * pixel_w)
            a = pixels[(i * 4) + 3]
            if a != 0.0:
                is_fill = True
                break

    if not is_fill:
        # print("skipping:", filepath)
        return

    with open(filepath, 'wb') as f:

        f.write(
            struct.pack(
                '<6I',
                sub_w, sub_h,
                sub_x, sub_y,
                # redundant but including to maintain consistency
                pixel_w, pixel_h,
            ))

        for y in range(sub_h):
            for x in range(sub_w):
                i = (sub_x + x) + ((sub_y + y) * pixel_w)
                rgba = pixels[(i * 4):(i * 4) + 4]
                c = sum((int(p * 255) << (8 * i)) for i, p in enumerate(rgba))
                f.write(struct.pack("<I", c))


_dice_icon_name_cache = {}


def dice_icon_name(
        x, y, parts_x, parts_y,
        name_style=None, prefix=""):
    """
    How to name icons, this is mainly for what name we get in git,
    the actual names don't really matter, its just nice to have the
    name match up with something recognizable for commits.
    """
    if name_style == 'UI_ICONS':

        # Init on demand
        if not _dice_icon_name_cache:
            import re
            count = 0

            # Search for eg: DEF_ICON(BRUSH_NUDGE) --> BRUSH_NUDGE
            re_icon = re.compile(r'^\s*DEF_ICON.*\(\s*([A-Za-z0-9_]+)\s*\).*$')

            ui_icons_h = os.path.join(SOURCE_DIR, "source", "blender", "editors", "include", "UI_icons.h")
            with open(ui_icons_h, 'r', encoding="utf-8") as f:
                for l in f:
                    match = re_icon.search(l)
                    if match:
                        if l.find('DEF_ICON_BLANK') == -1:
                            icon_name = match.group(1).lower()
                            print(icon_name)
                            _dice_icon_name_cache[count] = icon_name
                        count += 1
        # ---- Done with icon cache

        index = (y * parts_x) + x
        if index not in _dice_icon_name_cache:
            return None

        icon_name = _dice_icon_name_cache[index]

        # for debugging its handy to sort by number
        # ~ id_str = "%03d_%s%s.dat" % (index, prefix, icon_name)

        id_str = "%s%s.dat" % (prefix, icon_name)

    elif name_style == "":
        # flip so icons are numbered from top-left
        # because new icons will be added at the bottom
        y_flip = parts_y - (y + 1)
        id_str = "%s%02xx%02x.dat" % (prefix, x, y_flip)
    else:
        raise Exception("Invalid '--name_style' arg")

    return id_str


def dice(
        filepath, output, output_prefix, name_style,
        parts_x, parts_y,
        minx, miny, maxx, maxy,
        minx_icon, miny_icon, maxx_icon, maxy_icon,
        spacex_icon, spacey_icon,
):

    is_simple = (max(
        minx, miny, maxx, maxy,
        minx_icon, miny_icon, maxx_icon, maxy_icon,
        spacex_icon, spacey_icon) == 0)

    pixels, pixel_w, pixel_h = image_from_file(filepath)

    if not (pixel_w and pixel_h):
        print("Image not found %r!" % filepath)
        return

    if not os.path.exists(output):
        os.mkdir(output)

    if is_simple:
        pixels_w_clip = pixel_w
        pixels_h_clip = pixel_h

        icon_w = pixels_w_clip // parts_x
        icon_h = pixels_h_clip // parts_y
        icon_w_clip = icon_w
        icon_h_clip = icon_h
    else:
        pixels_w_clip = pixel_w - (minx + maxx)
        pixels_h_clip = pixel_h - (miny + maxy)

        icon_w = (pixels_w_clip - ((parts_x - 1) * spacex_icon)) // parts_x
        icon_h = (pixels_h_clip - ((parts_y - 1) * spacey_icon)) // parts_y
        icon_w_clip = icon_w - (minx_icon + maxx_icon)
        icon_h_clip = icon_h - (miny_icon + maxy_icon)

    print(pixel_w, pixel_h, icon_w, icon_h)

    for x in range(parts_x):
        for y in range(parts_y):
            id_str = dice_icon_name(
                x, y,
                parts_x, parts_y,
                name_style=name_style, prefix=output_prefix
            )
            if not id_str:
                continue

            filepath = os.path.join(output, id_str)
            if VERBOSE:
                print("  writing:", filepath)

            # simple, no margins
            if is_simple:
                sub_x = x * icon_w
                sub_y = y * icon_h
            else:
                sub_x = minx + ((x * (icon_w + spacex_icon)) + minx_icon)
                sub_y = miny + ((y * (icon_h + spacey_icon)) + miny_icon)

            write_subimage(sub_x, sub_y, icon_w_clip, icon_h_clip,
                           filepath,
                           pixels, pixel_w, pixel_h)


def main():
    import sys
    import argparse

    epilog = "Run this after updating the SVG file"

    argv = sys.argv

    if "--" not in argv:
        argv = []
    else:
        argv = argv[argv.index("--") + 1:]

    parser = argparse.ArgumentParser(description=__doc__, epilog=epilog)

    # File path options
    parser.add_argument(
        "--image", dest="image", metavar='FILE',
        help="Image file",
    )
    parser.add_argument(
        "--output", dest="output", metavar='DIR',
        help="Output directory",
    )
    parser.add_argument(
        "--output_prefix", dest="output_prefix", metavar='STRING',
        help="Output prefix",
    )

    # Icon naming option
    parser.add_argument(
        "--name_style", dest="name_style", metavar='ENUM', type=str,
        choices=('', 'UI_ICONS'),
        help="The method used for naming output data",
    )

    # Options for dicing up the image
    parser.add_argument(
        "--parts_x", dest="parts_x", metavar='INT', type=int,
        help="Grid X parts",
    )
    parser.add_argument(
        "--parts_y", dest="parts_y", metavar='INT', type=int,
        help="Grid Y parts",
    )

    _help = "Inset from the outer edge (in pixels)"
    parser.add_argument("--minx", dest="minx", metavar='INT', type=int, help=_help)
    parser.add_argument("--miny", dest="miny", metavar='INT', type=int, help=_help)
    parser.add_argument("--maxx", dest="maxx", metavar='INT', type=int, help=_help)
    parser.add_argument("--maxy", dest="maxy", metavar='INT', type=int, help=_help)

    _help = "Inset from each icons bounds (in pixels)"
    parser.add_argument("--minx_icon", dest="minx_icon", metavar='INT', type=int, help=_help)
    parser.add_argument("--miny_icon", dest="miny_icon", metavar='INT', type=int, help=_help)
    parser.add_argument("--maxx_icon", dest="maxx_icon", metavar='INT', type=int, help=_help)
    parser.add_argument("--maxy_icon", dest="maxy_icon", metavar='INT', type=int, help=_help)

    _help = "Empty space between icons"
    parser.add_argument("--spacex_icon", dest="spacex_icon", metavar='INT', type=int, help=_help)
    parser.add_argument("--spacey_icon", dest="spacey_icon", metavar='INT', type=int, help=_help)

    del _help

    args = parser.parse_args(argv)

    if not argv:
        print("No args given!")
        parser.print_help()
        return

    dice(args.image, args.output, args.output_prefix, args.name_style,
         args.parts_x, args.parts_y,
         args.minx, args.miny, args.maxx, args.maxy,
         args.minx_icon, args.miny_icon, args.maxx_icon, args.maxy_icon,
         args.spacex_icon, args.spacey_icon,
         )


if __name__ == "__main__":
    main()
