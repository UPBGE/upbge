#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Vendor the FidelityFX FSR 3.1 upscaler shaders into Blender shader libraries.

Reads the GPU headers of a FidelityFX SDK checkout (v1.1.4 = FSR 3.1.4) and writes one
`*_lib.glsl` per AMD header, in the dialect Blender's shader_tool accepts on every backend:
- the GLSL path of the headers, FP32 only (FFX_HALF 0), no wave operations;
- FidelityFX types and qualifiers replaced by Blender's (float2, `T &x` for inout and out);
- global constants turned into macros (MSL allocates them per thread);
- braces around unbraced statements;
- the includes of SPD and FSR1 inlined where the header includes them mid-file, reduced to what
  the including header uses;
- explicit conversions where the code relies on implicit vector conversions (MSL has none).

The only shader change of FSR 3.1.6 (SDK 2.x) samples the reprojected history with the previous
frame's upscale size in the FP16 path: the FP32 path vendored here already does.

The permutation options are fixed for EEVEE (see KNOWN) except APPLY_SHARPENING, which stays a
conditional like the binding guards (FSR3UPSCALER_BIND_*): the create-infos set them.

Usage: vendor_fsr3.py <fidelityfx-sdk/sdk/include/FidelityFX/gpu> <output dir>
  The output dir is shaders/fsr3; its parent holds eevee_game_fsr3_callbacks_lib.glsl.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppx  # noqa: E402
import shake  # noqa: E402

SDK_VERSION = "1.1.4"

KNOWN = {
    "FFX_GPU": 1,
    "FFX_GLSL": 1,
    "FFX_HALF": 0,
    "FFX_SPD_NO_WAVE_OPERATIONS": 1,
    # EEVEE's inputs, must match FSR3_OPTION_* in eevee_game_fsr3_shared.hh: scene linear
    # color, render resolution motion vectors without jitter, reversed depth. Lanczos computed,
    # not from the LUT (FidelityFX uses the LUT only on GPUs with both wave32 and wave64).
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT": 1,
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS": 1,
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS": 0,
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH": 1,
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE": 0,
    # Fixed by AMD's shader build for every permutation.
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE": 2,
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF": 0,
    "FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF": 0,
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF": 0,
    "FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF": 0,
}
OPEN_PREFIXES = ("FFX_FSR3UPSCALER_OPTION_", "FSR3UPSCALER_BIND_")

# (output name, AMD header, headers inlined at their #include)
FILES = [
    ("ffx_fsr3upscaler_common_lib.glsl", "fsr3upscaler/ffx_fsr3upscaler_common.h"),
    ("ffx_fsr3upscaler_sample_lib.glsl", "fsr3upscaler/ffx_fsr3upscaler_sample.h"),
    ("ffx_fsr3upscaler_upsample_lib.glsl", "fsr3upscaler/ffx_fsr3upscaler_upsample.h"),
    ("ffx_fsr3upscaler_reproject_lib.glsl", "fsr3upscaler/ffx_fsr3upscaler_reproject.h"),
    ("ffx_fsr3upscaler_accumulate_lib.glsl", "fsr3upscaler/ffx_fsr3upscaler_accumulate.h"),
    ("ffx_fsr3upscaler_prepare_inputs_lib.glsl", "fsr3upscaler/ffx_fsr3upscaler_prepare_inputs.h"),
    ("ffx_fsr3upscaler_luma_pyramid_lib.glsl", "fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h"),
    ("ffx_fsr3upscaler_shading_change_pyramid_lib.glsl",
     "fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid.h"),
    ("ffx_fsr3upscaler_shading_change_lib.glsl", "fsr3upscaler/ffx_fsr3upscaler_shading_change.h"),
    ("ffx_fsr3upscaler_prepare_reactivity_lib.glsl",
     "fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity.h"),
    ("ffx_fsr3upscaler_luma_instability_lib.glsl",
     "fsr3upscaler/ffx_fsr3upscaler_luma_instability.h"),
    ("ffx_fsr3upscaler_rcas_lib.glsl", "fsr3upscaler/ffx_fsr3upscaler_rcas.h"),
]
INLINED = {"spd/ffx_spd.h", "fsr1/ffx_fsr1.h"}
# Hand-written files whose use of ffx_core functions must be kept.
EXTRA_CORE_USERS = ("eevee_game_fsr3_callbacks_lib.glsl",)

# Type and qualifier tokens of ffx_common_types.h (GLSL path, FFX_HALF 0) -> Blender GLSL.
TYPES = {
    "FfxBoolean": "bool",
    "FfxFloat32": "float", "FfxFloat32x2": "float2", "FfxFloat32x3": "float3",
    "FfxFloat32x4": "float4",
    "FfxUInt32": "uint", "FfxUInt32x2": "uint2", "FfxUInt32x3": "uint3", "FfxUInt32x4": "uint4",
    "FfxInt32": "int", "FfxInt32x2": "int2", "FfxInt32x3": "int3", "FfxInt32x4": "int4",
    "FfxFloat32x4x4": "float4x4", "FfxFloat32x3x4": "float4x3", "FfxFloat32x3x3": "float3x3",
    "FfxFloat32x2x2": "float2x2", "FfxFloat32Mat4": "float4x4", "FfxFloat32Mat3": "float3x3",
    "FFX_MIN16_F": "float", "FFX_MIN16_F2": "float2", "FFX_MIN16_F3": "float3",
    "FFX_MIN16_F4": "float4",
    "FFX_MIN16_I": "int", "FFX_MIN16_I2": "int2", "FFX_MIN16_I3": "int3", "FFX_MIN16_I4": "int4",
    "FFX_MIN16_U": "uint", "FFX_MIN16_U2": "uint2", "FFX_MIN16_U3": "uint3",
    "FFX_MIN16_U4": "uint4",
    "FFX_16BIT_F": "float", "FFX_16BIT_F2": "float2", "FFX_16BIT_F3": "float3",
    "FFX_16BIT_F4": "float4",
    "FFX_GROUPSHARED": "shared",
    "FFX_TRUE": "true", "FFX_FALSE": "false",
}
# Tokens removed (empty in the GLSL path).
EMPTY = ("FFX_STATIC", "FFX_UNROLL", "FFX_PREFER_WAVE64", "FFX_PARAMETER_IN")
# Function-like macros of ffx_core_glsl.h, expanded here so shader_tool sees plain GLSL.
FUNC_MACROS = {
    "FFX_GREATER_THAN": "greaterThan", "FFX_GREATER_THAN_EQUAL": "greaterThanEqual",
    "FFX_LESS_THAN": "lessThan", "FFX_LESS_THAN_EQUAL": "lessThanEqual",
    "FFX_EQUAL": "equal", "FFX_NOT_EQUAL": "notEqual",
    "FFX_BROADCAST_FLOAT32": "float", "FFX_BROADCAST_FLOAT32X2": "float2",
    "FFX_BROADCAST_FLOAT32X3": "float3", "FFX_BROADCAST_FLOAT32X4": "float4",
    "FFX_BROADCAST_UINT32": "uint", "FFX_BROADCAST_UINT32X2": "uint2",
    "FFX_BROADCAST_UINT32X3": "uint3", "FFX_BROADCAST_UINT32X4": "uint4",
    "FFX_BROADCAST_INT32": "int", "FFX_BROADCAST_INT32X2": "int2",
    "FFX_BROADCAST_INT32X3": "int3", "FFX_BROADCAST_INT32X4": "int4",
    "FFX_BROADCAST_MIN_FLOAT16": "float", "FFX_BROADCAST_MIN_FLOAT16X2": "float2",
    "FFX_BROADCAST_MIN_FLOAT16X3": "float3", "FFX_BROADCAST_MIN_FLOAT16X4": "float4",
}

HEADER = """\
/* SPDX-FileCopyrightText: 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT */

/**
 * Generated by vendor_fsr3.py from FidelityFX SDK v{version}, {source}.
 * Do not edit: change the script and run it again.
 */

#pragma once
"""


def read(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    return text.replace("\r\n", "\n").replace("\t", "    ")


def strip_license(text):
    """Remove the leading // comment block (AMD's MIT notice, restated by the SPDX header)."""
    lines = text.split("\n")
    i = 0
    while i < len(lines) and (lines[i].startswith("//") or lines[i].strip() == ""):
        i += 1
    return "\n".join(lines[i:])


INLINE_BEGIN = "// @@inline-begin"
INLINE_END = "// @@inline-end"


def inline_includes(text, gpu_dir):
    """Inline the headers of INLINED between markers, drop the other includes."""
    def repl(m):
        name = m.group(1)
        if name in INLINED:
            body = strip_license(read(os.path.join(gpu_dir, name)))
            return "%s %s\n%s\n%s" % (INLINE_BEGIN, name, body, INLINE_END)
        return ""
    return re.sub(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"[^\n]*$', repl, text, flags=re.M)


def shake_inlined(text):
    """Keep in each inlined header only what the including header uses."""
    pattern = re.compile(r"^%s ([^\n]+)\n(.*?)^%s$" % (INLINE_BEGIN, INLINE_END), re.M | re.S)
    outside = shake.identifiers(pattern.sub("", text))
    return pattern.sub(lambda m: "/* Inlined from %s. */\n%s" % (
        m.group(1), shake.shake(m.group(2), outside)), text)


_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


def code_sub(pattern, repl, text):
    """re.sub outside comments."""
    parts = []
    last = 0
    for m in _COMMENT.finditer(text):
        parts.append(re.sub(pattern, repl, text[last:m.start()]))
        parts.append(m.group(0))
        last = m.end()
    parts.append(re.sub(pattern, repl, text[last:]))
    return "".join(parts)


def rewrite_tokens(text):
    # Parameter qualifiers: Blender GLSL uses C++ references for inout and out.
    text = code_sub(r"\b(?:FFX_PARAMETER_INOUT|FFX_PARAMETER_OUT|inout|out)\s+(\w+)\s+(\w+)",
                    r"\1 &\2", text)
    for k, v in FUNC_MACROS.items():
        text = re.sub(r"\b%s\b" % k, v, text)
    for k, v in TYPES.items():
        text = re.sub(r"\b%s\b" % k, v, text)
    for k in EMPTY:
        text = re.sub(r"[ \t]*\b%s\b[ \t]*" % k, " ", text)
    # Statement-like macros.
    # groupMemoryBarrier() does not exist in Blender's MSL: in compute shaders barrier() also
    # orders the shared memory accesses (GLSL 4.30), which is all FidelityFX needs it for.
    text = re.sub(r"\bFFX_GROUP_MEMORY_BARRIER\b", "barrier()", text)
    # HLSL-only pragmas.
    text = re.sub(r"^[ \t]*#[ \t]*pragma[ \t]+warning[^\n]*\n", "", text, flags=re.M)
    # Line starts left by removed tokens.
    text = re.sub(r"(?m)^ +(?=\S)", lambda m: m.group(0) if len(m.group(0)) > 1 else "", text)
    return text


_GLOBAL_CONST = re.compile(r"^const\s+(\w+)\s+(\w+)\s*=\s*([^;]+);[ \t]*(//[^\n]*)?$", re.M)


def global_constants_to_macros(text):
    """`const T NAME = EXPR;` at global scope -> `#define NAME T(EXPR)`."""
    out = []
    depth = 0
    for line in text.split("\n"):
        m = _GLOBAL_CONST.match(line.strip()) if depth == 0 and not line.startswith((" ", "#")) else None
        if m:
            comment = (" " + m.group(4).replace("//", "/*", 1) + " */") if m.group(4) else ""
            value = m.group(3).strip()
            if m.group(1) in ("int", "uint") and re.fullmatch(r"\d+", value):
                # A bare literal keeps the type and stays a valid array size: the NVIDIA GLSL
                # compiler rejects a constructor in an array size inside a struct (C7546).
                out.append("#define %s %s%s%s" % (m.group(2), value, "u" if m.group(1) == "uint" else "",
                                                  comment))
            else:
                out.append("#define %s %s(%s)%s" % (m.group(2), m.group(1), value, comment))
        else:
            out.append(line)
        if not line.lstrip().startswith("#"):
            code = re.sub(r"//.*$", "", line)
            depth += code.count("{") - code.count("}")
    return "\n".join(out)


def _skip_space_and_comments(text, i):
    while True:
        while i < len(text) and text[i].isspace():
            i += 1
        if text.startswith("//", i):
            i = text.index("\n", i)
        elif text.startswith("/*", i):
            i = text.index("*/", i) + 2
        else:
            return i


def _matching(text, i, open_c, close_c):
    depth = 0
    while True:
        c = text[i]
        if c == open_c:
            depth += 1
        elif c == close_c:
            depth -= 1
            if depth == 0:
                return i
        i += 1


def brace_statements(text):
    """Blender lints `if (c) x;`: wrap unbraced if, for, while and else bodies in braces."""
    out = []
    i = 0
    keyword = re.compile(r"\b(if|for|while|else)\b")
    while True:
        m = keyword.search(text, i)
        if not m:
            out.append(text[i:])
            break
        line_start = text.rfind("\n", 0, m.start()) + 1
        line = text[line_start:m.start()]
        if line.lstrip().startswith(("#", "//", "*", "/*")):
            out.append(text[i:m.end()])
            i = m.end()
            continue
        j = m.end()
        if m.group(1) != "else":
            j = _skip_space_and_comments(text, j)
            if text[j] != "(":
                out.append(text[i:m.end()])
                i = m.end()
                continue
            j = _matching(text, j, "(", ")") + 1
        k = _skip_space_and_comments(text, j)
        if text[k] == "{" or (m.group(1) == "else" and text.startswith("if", k)) or text[k] == ";":
            out.append(text[i:j])
            i = j
            continue
        assert not keyword.match(text, k), "nested unbraced control statement"
        end = k
        depth = 0
        while not (text[end] == ";" and depth == 0):
            depth += {"(": 1, ")": -1}.get(text[end], 0)
            end += 1
        indent = re.match(r"[ \t]*", text[line_start:]).group(0)
        body = text[k:end + 1]
        out.append(text[i:j] + " {\n" + indent + "    " + body + "\n" + indent + "}")
        i = end + 1
    return "".join(out)


def fix_tokens(text):
    """Spellings Blender's shader_tool does not parse."""
    # `a)>1.f`: split the comparison from the parenthesis.
    text = re.sub(r"\)(>|<)(?=[\w.])", r") \1 ", text)
    # `const` on struct parameters.
    text = re.sub(r"(?<=[(,])(\s*)const\s+(?=\w+\s+&?\w+\s*[,)])", r"\1", text)
    return text


def float_literals(text):
    """`1.0` -> `1.0f`: unsuffixed literals are double in C++ (shader C++ compilation)."""
    lit = r"(?<![\w.])(\d+\.\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)(?![\w.])"
    lines = []
    for line in text.split("\n"):
        if re.match(r"\s*#\s*(if|elif)", line):
            lines.append(line)
        else:
            lines.append(code_sub(lit, r"\1f", line))
    return "\n".join(lines)


# Explicit conversions where FidelityFX relies on GLSL's implicit ones: MSL has none, and C++
# (and MSL) turn `int2 * 0.5f` into an integer operation. The casts are the conversions GLSL
# performs, the results do not change. (file, pattern, replacement, expected matches)
_SIZE = r"(RenderSize|UpscaleSize|PreviousFrameRenderSize|PreviousFrameUpscaleSize|" \
        r"ShadingChangeRenderSize|MaxRenderSize)\(\)"
METAL_PATCHES = [
    # Integer pixel position plus half a pixel, divided or scaled by integer sizes.
    ("*", r"\((iPx\w*) \+ (0\.5f|float\(0\.5f\))\)", r"(float2(\1) + \2)", None),
    ("*", r"([*/]) %s" % _SIZE, r"\1 float2(\2())", None),
    ("ffx_fsr3upscaler_common_lib.glsl", r"int2\((\w+)\(\) \* 0\.5f\)",
     r"int2(float2(\1()) * 0.5f)", 3),
    ("ffx_fsr3upscaler_common_lib.glsl", r"fUv \* iTextureSize", "fUv * float2(iTextureSize)", 1),
    ("ffx_fsr3upscaler_common_lib.glsl", r"fUv \* iSize", "fUv * float2(iSize)", 1),
    ("ffx_fsr3upscaler_common_lib.glsl", r"ComputeNdc\(iViewportPos,", "ComputeNdc(float2(iViewportPos),", 1),
    # Scalar swizzles.
    ("ffx_fsr3upscaler_common_lib.glsl",
     r"fRgb / \((ffxMax\(ffxMax\(0\.f, fRgb\.r\), ffxMax\(fRgb\.g, fRgb\.b\)\) \+ 1\.f)\)\.xxx",
     r"fRgb / float3(\1)", 1),
    ("ffx_fsr3upscaler_common_lib.glsl",
     r"fRgb / (ffxMax\(FSR3UPSCALER_TONEMAP_EPSILON, 1\.f - ffxMax\(fRgb\.r, ffxMax\(fRgb\.g, fRgb\.b\)\)\))\.xxx",
     r"fRgb / float3(\1)", 1),
    ("ffx_fsr3upscaler_upsample_lib.glsl", r"fKernelWeight\.xx", "float2(fKernelWeight)", 1),
    # SPD: integer texel coordinates into the unsigned and float parameters of the callbacks.
    ("ffx_fsr3upscaler_*pyramid_lib.glsl", r"SpdLoadSourceImage\(int2\((i\d)\), slice\)",
     r"SpdLoadSourceImage(float2(\1), slice)", 4),
    ("ffx_fsr3upscaler_*pyramid_lib.glsl", r"SpdReduceLoadSourceImage\(tex, slice\)",
     r"SpdReduceLoadSourceImage(uint2(tex), slice)", 4),
    ("ffx_fsr3upscaler_*pyramid_lib.glsl", r"SpdReduceLoad4\(tex, slice\)",
     r"SpdReduceLoad4(uint2(tex), slice)", 4),
]
# Performance: the shading change pyramid indexes its sample sets with indices known only at
# run time, which keeps them in local memory (10x the cost of the luma pyramid on NVIDIA).
# Selects instead, bit-exact. (file, pattern, replacement, expected matches)
_SAMPLE_SET_AT = """\
// Element i of a sample set through selects: an index known only at run time makes the
// compiler keep the arrays in local memory, which made this pass cost ten times the luma
// pyramid on NVIDIA. Bit-exact: i is in [0, 4] wherever the value is used.
float SampleSetAt(SampleSet fSet, int i)
{
    return (i == 0) ? fSet.fSamples[0] : (i == 1) ? fSet.fSamples[1] : (i == 2) ? fSet.fSamples[2] :
           (i == 3) ? fSet.fSamples[3] : fSet.fSamples[4];
}

float ComputeMinimumDifference("""
PERF_PATCHES = [
    ("ffx_fsr3upscaler_shading_change_pyramid_lib.glsl", r"float ComputeMinimumDifference\(",
     _SAMPLE_SET_AT, 1),
    ("ffx_fsr3upscaler_shading_change_pyramid_lib.glsl", r"fSet0\.fSamples\[a\]",
     "SampleSetAt(fSet0, a)", 4),
    ("ffx_fsr3upscaler_shading_change_pyramid_lib.glsl", r"fSet1\.fSamples\[b\]",
     "SampleSetAt(fSet1, b)", 4),
]
# ffx_core: FidelityFX's HLSL implementation of the bit field helpers (Blender's MSL has no
# bitfieldInsert).
CORE_PATCHES = [
    (r"return bitfieldExtract\(src, int\(off\), int\(bits\)\);",
     "uint mask = (1u << bits) - 1u;\n    return (src >> off) & mask;", 1),
    (r"return bitfieldInsert\(src, ins, 0, int\(bits\)\);",
     "uint mask = (1u << bits) - 1u;\n    return (ins & mask) | (src & (~mask));", 1),
]


def apply_patches(name, text, patches):
    import fnmatch
    for pat_file, pattern, repl, count in patches:
        if not fnmatch.fnmatch(name, pat_file):
            continue
        text, n = re.subn(pattern, repl, text)
        if count is not None and n != count:
            sys.exit("%s: patch %r matched %d times, expected %d" % (name, pattern, n, count))
    return text


def tidy(text):
    text = re.sub(r"\n{3,}", "\n\n", text)
    text = re.sub(r"[ \t]+\n", "\n", text)
    return text.strip("\n") + "\n"


def vendor_file(gpu_dir, source, macros):
    text = read(os.path.join(gpu_dir, source))
    text = strip_license(text)
    text = inline_includes(text, gpu_dir)
    text = ppx.process(text, macros)
    text = rewrite_tokens(text)
    text = global_constants_to_macros(text)
    text = shake_inlined(text)
    text = drop_header_guards(text)
    text = substitute_known_options(text)
    text = brace_statements(text)
    text = fix_tokens(text)
    text = float_literals(text)
    return tidy(text)


def substitute_known_options(text):
    """The options fixed here are no longer defined: substitute their value where used."""
    for name, value in KNOWN.items():
        if name.startswith("FFX_FSR3UPSCALER_OPTION_"):
            text = code_sub(r"\b%s\b" % name, str(value), text)
    return text


def drop_header_guards(text):
    return re.sub(r"(?m)^#define \w+_H\s*\n", "", text)


CORE_SOURCES = ("ffx_core_glsl.h", "ffx_core_gpu_common.h", "ffx_core_portability.h")


def vendor_core(gpu_dir, roots):
    """The subset of ffx_core used by the vendored headers, GLSL path, FP32."""
    macros = ppx.Macros(KNOWN, OPEN_PREFIXES)
    text = "\n".join(strip_license(read(os.path.join(gpu_dir, s))) for s in CORE_SOURCES)
    text = re.sub(r'(?m)^[ \t]*#[ \t]*(include|extension)[^\n]*$', "", text)
    text = ppx.process(text, macros)
    # The macros rewrite_tokens() expands must not survive as definitions.
    rewritten = set(TYPES) | set(FUNC_MACROS) | set(EMPTY) | {"FFX_GROUP_MEMORY_BARRIER"}
    text = re.sub(r"(?m)^#define (\w+)\b[^\n]*\n",
                  lambda m: "" if m.group(1) in rewritten else m.group(0), text)
    # The core is plumbing (ffxMax is max): its documentation is AMD's, not needed here.
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"(?m)^[ \t]*//[^\n]*\n", "", text)
    text = rewrite_tokens(text)
    text = global_constants_to_macros(text)
    text = shake.shake(text, roots)
    text = drop_header_guards(text)
    text = brace_statements(text)
    text = fix_tokens(text)
    text = float_literals(text)
    text = apply_patches("ffx_core_lib.glsl", text, [("*",) + p for p in CORE_PATCHES])
    return tidy(text)


def main():
    gpu_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    macros = ppx.Macros(KNOWN, OPEN_PREFIXES)
    outputs = []
    for out_name, source in FILES:
        body = apply_patches(out_name, vendor_file(gpu_dir, source, macros), METAL_PATCHES)
        body = apply_patches(out_name, body, PERF_PATCHES)
        outputs.append((out_name, source, body))
    # The core keeps what the vendored headers and our callbacks use.
    roots = set()
    for _, _, body in outputs:
        roots |= shake.identifiers(body)
    for extra in EXTRA_CORE_USERS:
        path = os.path.join(out_dir, os.pardir, extra)
        if os.path.exists(path):
            roots |= shake.identifiers(read(path))
    outputs.insert(0, ("ffx_core_lib.glsl", ", ".join(CORE_SOURCES), vendor_core(gpu_dir, roots)))
    for out_name, source, body in outputs:
        with open(os.path.join(out_dir, out_name), "w", encoding="utf-8") as f:
            f.write(HEADER.format(version=SDK_VERSION, source=source) + "\n" + body)
        print(out_name, body.count("\n"), "lines")


if __name__ == "__main__":
    main()
