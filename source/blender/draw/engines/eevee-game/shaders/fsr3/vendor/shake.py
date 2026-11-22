# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Dead code removal at global scope for preprocessed GLSL: keeps the functions, structs, macros
and shared variables reachable from a set of root identifiers.
"""

import re

_IDENT = re.compile(r"[A-Za-z_]\w*")


def _strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def split_items(text):
    """Split into global items: dict(kind, name, text). Comments stick to the next item."""
    items = []
    lines = text.split("\n")
    i = 0
    pending = []
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if s == "" or s.startswith("//") or s.startswith("/*") and s.endswith("*/"):
            pending.append(line)
            i += 1
            continue
        if s.startswith("/*"):
            while not lines[i].strip().endswith("*/"):
                pending.append(lines[i])
                i += 1
            pending.append(lines[i])
            i += 1
            continue
        if s.startswith("#"):
            group = [line]
            while group[-1].rstrip().endswith("\\"):
                i += 1
                group.append(lines[i])
            i += 1
            m = re.match(r"#\s*define\s+(\w+)", s)
            kind = "define" if m else "directive"
            items.append(dict(kind=kind, name=m.group(1) if m else None,
                              text="\n".join(pending + group)))
            pending = []
            continue
        # Code item: accumulate until the braces close and a ';' or '}' ends it.
        group = []
        depth = 0
        seen_brace = False
        while i < len(lines):
            group.append(lines[i])
            code = _strip_comments(lines[i])
            depth += code.count("{") - code.count("}")
            seen_brace |= "{" in code
            i += 1
            stripped = code.strip()
            if depth == 0 and (stripped.endswith(";") or (seen_brace and stripped.endswith("}"))):
                break
        body = _strip_comments("\n".join(group))
        head = body.split("{", 1)[0]
        kind, name = "other", None
        m = re.match(r"\s*struct\s+(\w+)", head)
        if m:
            kind, name = "struct", m.group(1)
        elif "(" in head and (seen_brace or head.rstrip().endswith(");")):
            m = re.search(r"(\w+)\s*\($", head.split("(", 1)[0] + "(")
            name = m.group(1) if m else None
            kind = "function" if seen_brace else "prototype"
        else:
            m = re.match(r"\s*shared\s+\w+\s+(\w+)", head)
            if m:
                kind, name = "shared", m.group(1)
        items.append(dict(kind=kind, name=name, text="\n".join(pending + group)))
        pending = []
    if pending:
        items.append(dict(kind="other", name=None, text="\n".join(pending)))
    return items


def identifiers(s):
    return set(_IDENT.findall(_strip_comments(s)))


def shake(text, roots, drop_prototypes=True):
    """Keep items reachable from the root identifiers. Directives are always kept."""
    items = split_items(text)
    by_name = {}
    for it in items:
        if it["name"]:
            by_name.setdefault(it["name"], []).append(it)
    live = set()
    work = list(roots)
    while work:
        n = work.pop()
        if n in live:
            continue
        live.add(n)
        for it in by_name.get(n, []):
            work.extend(identifiers(it["text"]) - live)
    out = []
    for it in items:
        if it["kind"] == "prototype" and drop_prototypes:
            continue
        if it["name"] is None or it["name"] in live:
            out.append(it["text"])
    return "\n".join(out)
