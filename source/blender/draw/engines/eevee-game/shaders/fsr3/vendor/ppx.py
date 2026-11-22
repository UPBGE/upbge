# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Partial C preprocessor: resolves the conditionals whose value is known and keeps the others.

Known macros have a fixed value (or are known undefined). Macros matching one of the open
prefixes are left to the shader compiler: their conditionals are kept verbatim. Any other
macro is undefined unless the processed text defines it (closed world).
"""

import itertools
import re

_TOKEN = re.compile(r"\s*(defined|[A-Za-z_]\w*|0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*|&&|\|\||==|!=|<=|>=|<<|>>|[!<>()+\-*/%&|^~?:])")

UNDEF = object()


class Macros:
    def __init__(self, known, open_prefixes):
        # name -> int value, or UNDEF
        self.known = dict(known)
        self.open_prefixes = tuple(open_prefixes)
        self.open_names = set()

    def is_open(self, name):
        if name in self.known:
            return False
        return name in self.open_names or name.startswith(self.open_prefixes)

    def value(self, name):
        if self.is_open(name):
            return None
        return self.known.get(name, UNDEF)

    def define(self, name, text):
        if name.startswith(self.open_prefixes) and name not in self.known:
            return
        text = text.strip()
        m = re.fullmatch(r"\(?\s*(-?\d+)[uU]?\s*\)?", text)
        if text == "":
            self.known[name] = 1
        elif m:
            self.known[name] = int(m.group(1))
        else:
            # Non-integer macro: defined, value not usable in #if arithmetic.
            self.known[name] = 1

    def undef(self, name):
        if not self.is_open(name):
            self.known[name] = UNDEF

    def make_open(self, name):
        self.known.pop(name, None)
        self.open_names.add(name)


def _tokenize(expr):
    pos = 0
    toks = []
    expr = expr.strip()
    while pos < len(expr):
        m = _TOKEN.match(expr, pos)
        if not m:
            raise ValueError(f"cannot tokenize '{expr}' at {pos}")
        toks.append(m.group(1))
        pos = m.end()
        while pos < len(expr) and expr[pos].isspace():
            pos += 1
    return toks


def evaluate(expr, macros):
    """Return True, False or None (depends on open macros)."""
    expr = re.sub(r"//.*$", "", expr)
    expr = re.sub(r"/\*.*?\*/", "", expr)
    toks = _tokenize(expr)
    # Collect open identifiers.
    opens = []
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == "defined":
            name = toks[i + 2] if toks[i + 1] == "(" else toks[i + 1]
            if macros.is_open(name) and ("D", name) not in opens:
                opens.append(("D", name))
            i += 4 if toks[i + 1] == "(" else 2
            continue
        if re.match(r"[A-Za-z_]", t) and macros.is_open(t) and ("V", t) not in opens:
            opens.append(("V", t))
        i += 1

    def to_python(assign):
        out = []
        i = 0
        while i < len(toks):
            t = toks[i]
            if t == "defined":
                name = toks[i + 2] if toks[i + 1] == "(" else toks[i + 1]
                i += 4 if toks[i + 1] == "(" else 2
                if ("D", name) in assign:
                    out.append("1" if assign[("D", name)] else "0")
                else:
                    out.append("0" if macros.value(name) is UNDEF else "1")
                continue
            if re.match(r"[A-Za-z_]", t):
                if ("V", t) in assign:
                    out.append(str(assign[("V", t)]))
                else:
                    v = macros.value(t)
                    out.append("0" if v is UNDEF else str(v))
            elif re.match(r"\d|0[xX]", t):
                out.append(str(int(re.sub(r"[uUlL]+$", "", t), 0)))
            elif t == "&&":
                out.append(" and ")
            elif t == "||":
                out.append(" or ")
            elif t == "!":
                out.append(" not ")
            elif t == "/":
                out.append("//")
            else:
                out.append(t)
            i += 1
        return "".join(out)

    results = set()
    choices = []
    for kind, _ in opens:
        choices.append((0, 1) if kind == "D" else (0, 1, 2))
    for combo in itertools.product(*choices):
        assign = dict(zip(opens, combo))
        try:
            results.add(bool(eval(to_python(assign), {}, {})))
        except ZeroDivisionError:
            results.add(False)
        if len(results) > 1:
            return None
    return results.pop()


_DIRECTIVE = re.compile(r"^\s*#\s*(\w+)\s*(.*)$")


def logical_lines(text):
    """Yield (physical_lines, logical_text) joining backslash continuations."""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        group = [lines[i]]
        while group[-1].rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            group.append(lines[i])
        i += 1
        logical = " ".join(l.rstrip()[:-1] if l.rstrip().endswith("\\") else l for l in group)
        yield group, logical


def process(text, macros):
    """Partially preprocess text. Returns the processed text."""
    out = []
    # Frame: dict(mode='known'|'open', active=bool, taken=bool, parent=bool)
    stack = []

    def emitting():
        return all(f["active"] for f in stack)

    def in_open_context():
        return any(f["mode"] == "open" for f in stack)

    for group, logical in logical_lines(text):
        m = _DIRECTIVE.match(logical)
        if not m:
            if emitting():
                out.extend(group)
            continue
        name, rest = m.group(1), m.group(2)
        rest = re.sub(r"//.*$", "", rest).strip()
        if name in ("if", "ifdef", "ifndef"):
            parent = emitting()
            if name == "if":
                val = evaluate(rest, macros) if parent else False
            else:
                ident = rest.split()[0]
                v = macros.value(ident)
                if v is None:
                    val = None
                else:
                    val = (v is not UNDEF) if name == "ifdef" else (v is UNDEF)
            if not parent:
                stack.append(dict(mode="known", active=False, taken=True))
            elif val is None:
                out.extend(group)
                stack.append(dict(mode="open", active=True, taken=False, dropped=False))
            else:
                stack.append(dict(mode="known", active=val, taken=val))
            continue
        if name in ("elif", "else"):
            f = stack[-1]
            parent = all(x["active"] for x in stack[:-1])
            if name == "elif":
                val = evaluate(rest, macros) if parent else False
            else:
                val = True
            if f["mode"] == "known":
                if f["taken"] or not parent:
                    f["active"] = False
                elif val is None:
                    # First undecided branch: becomes an #if of its own.
                    out.append(re.sub(r"#\s*elif", "#if", group[0], count=1))
                    out.extend(group[1:])
                    stack[-1] = dict(mode="open", active=True, taken=False, dropped=False)
                else:
                    f["active"] = val
                    f["taken"] = val
            else:
                if f["dropped"]:
                    f["active"] = False
                elif val is False:
                    f["active"] = False
                elif val is True:
                    out.append(re.sub(r"#\s*(elif.*|else.*)$", "#else", group[0]))
                    f["active"] = True
                    f["dropped"] = True  # Everything after this branch is dead.
                    f["last_else"] = True
                else:
                    out.extend(group)
                    f["active"] = True
            continue
        if name == "endif":
            f = stack.pop()
            if f["mode"] == "open":
                out.extend(group)
            continue
        if not emitting():
            continue
        if name == "define":
            dm = re.match(r"(\w+)(\([^)]*\))?\s*(.*)$", rest)
            mname = dm.group(1)
            if in_open_context():
                macros.make_open(mname)
            elif dm.group(2) is None:
                macros.define(mname, dm.group(3))
            else:
                macros.define(mname, "x")
        elif name == "undef":
            if in_open_context():
                macros.make_open(rest.split()[0])
            else:
                macros.undef(rest.split()[0])
        out.extend(group)
    assert not stack, "unterminated #if"
    return "\n".join(out)
