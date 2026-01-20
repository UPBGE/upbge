#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 UPBGE
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installs V8 via vcpkg (built with your MSVC) and copies the result to
# lib/windows_x64/v8/{include,lib}. Use this for best MSVC/header compatibility.
#
# Prerequisites:
#   - vcpkg: https://vcpkg.io/en/docs/README.html
#     git clone https://github.com/microsoft/vcpkg
#     .\vcpkg\bootstrap-vcpkg.bat
#   - VCPKG_ROOT set to the vcpkg directory, or vcpkg in PATH
#
# Usage (from project root):
#   python build_files/setup_v8_vcpkg.py
#   python build_files/setup_v8_vcpkg.py --vcpkg "C:\vcpkg" --output lib/windows_x64/v8
#

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

try:
    from urllib.request import urlopen, Request
    from urllib.error import URLError, HTTPError
except ImportError:
    from urllib2 import urlopen, Request, URLError, HTTPError  # type: ignore


def find_project_root() -> "Path":
    root = Path(__file__).resolve().parent.parent
    if (root / "CMakeLists.txt").exists() and (root / "build_files").exists():
        return root
    return Path.cwd()


def find_vcpkg(vcpkg_arg: "Path | None") -> "Path | None":
    if vcpkg_arg and vcpkg_arg.exists():
        exe = vcpkg_arg / "vcpkg.exe" if (vcpkg_arg / "vcpkg.exe").exists() else vcpkg_arg
        if exe.is_file() or (vcpkg_arg / "vcpkg.exe").exists():
            return vcpkg_arg.resolve()
    env = os.environ.get("VCPKG_ROOT")
    if env:
        p = Path(env)
        if (p / "vcpkg.exe").exists() or (p / "vcpkg").exists():
            return p.resolve()
    # try PATH
    which = "where" if sys.platform == "win32" else "which"
    try:
        out = subprocess.run([which, "vcpkg"], capture_output=True, text=True, timeout=5)
        if out.returncode == 0 and out.stdout.strip():
            exe = Path(out.stdout.strip().splitlines()[0].strip())
            return exe.parent.resolve()
    except Exception:
        pass
    return None


def run_cmd(cmd: "list[str]", cwd: "Path | None" = None) -> bool:
    try:
        subprocess.run(cmd, cwd=cwd, check=True, timeout=3600)
        return True
    except (subprocess.CalledProcessError, FileNotFoundError, OSError) as e:
        print(f"[V8 vcpkg] Command failed: {e}", file=sys.stderr)
        return False


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Install V8 via vcpkg (MSVC-built) and copy to lib/windows_x64/v8."
    )
    ap.add_argument(
        "--vcpkg",
        type=Path,
        default=None,
        help="vcpkg root directory (or set VCPKG_ROOT)",
    )
    ap.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output directory (default: <root>/lib/windows_x64/v8)",
    )
    ap.add_argument(
        "--triplet",
        default="x64-windows",
        help="vcpkg triplet (default: x64-windows)",
    )
    ap.add_argument(
        "--no-install",
        action="store_true",
        help="Only copy from existing vcpkg installed dir; do not run vcpkg install",
    )
    args = ap.parse_args()

    root = find_project_root()
    out = (args.output or (root / "lib" / "windows_x64" / "v8")).resolve()

    vcpkg_root = find_vcpkg(args.vcpkg)
    if not vcpkg_root:
        print(
            "[V8 vcpkg] vcpkg not found. Set VCPKG_ROOT or pass --vcpkg, or add vcpkg to PATH.",
            file=sys.stderr,
        )
        print("  https://vcpkg.io/en/docs/README.html", file=sys.stderr)
        return 1

    vcpkg_exe = vcpkg_root / "vcpkg.exe"
    if not vcpkg_exe.exists():
        vcpkg_exe = vcpkg_root / "vcpkg"
    if not vcpkg_exe.exists():
        # might be that vcpkg_root is the executable
        if vcpkg_root.suffix == ".exe" or vcpkg_root.name == "vcpkg":
            vcpkg_exe = vcpkg_root
            vcpkg_root = vcpkg_exe.parent
        else:
            print(f"[V8 vcpkg] vcpkg executable not found in {vcpkg_root}", file=sys.stderr)
            return 1

    installed = vcpkg_root / "installed" / args.triplet
    if not args.no_install:
        print(f"[V8 vcpkg] Running: {vcpkg_exe} install v8:{args.triplet}")
        if not run_cmd([str(vcpkg_exe), "install", f"v8:{args.triplet}"]):
            return 1
        print("[V8 vcpkg] vcpkg install finished.")

    if not installed.exists():
        print(f"[V8 vcpkg] Installed dir not found: {installed}", file=sys.stderr)
        return 1

    inc_src = installed / "include"
    lib_src = installed / "lib"
    bin_src = installed / "bin"

    out_include = out / "include"
    out_lib = out / "lib"

    out.mkdir(parents=True, exist_ok=True)
    if out_include.exists():
        shutil.rmtree(out_include)
    if out_lib.exists():
        shutil.rmtree(out_lib)
    out_include.mkdir(parents=True)
    out_lib.mkdir(parents=True)

    # Copy include (keep structure: include/..., include/v8/..., include/libplatform/...)
    if inc_src.exists():
        for f in inc_src.rglob("*"):
            if f.is_file():
                rel = f.relative_to(inc_src)
                dst = out_include / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, dst)
        print(f"[V8 vcpkg] Copied include from {inc_src}")
    else:
        print(f"[V8 vcpkg] Warning: no include at {inc_src}", file=sys.stderr)

    # Copy .lib
    if lib_src.exists():
        for f in lib_src.glob("*.lib"):
            shutil.copy2(f, out_lib)
        print(f"[V8 vcpkg] Copied .lib from {lib_src}")

    # Copy .dll from lib and bin (vcpkg may put them in either)
    for d in (lib_src, bin_src):
        if d.exists():
            for f in d.glob("*.dll"):
                shutil.copy2(f, out_lib)
    dlls = list(out_lib.glob("*.dll"))
    if dlls:
        print(f"[V8 vcpkg] Copied {len(dlls)} DLL(s) to {out_lib}")

    # Sanity: need v8.h and at least one .lib
    v8h = next(out_include.rglob("v8.h"), None)
    if not v8h:
        print("[V8 vcpkg] Warning: v8.h not found in output include.", file=sys.stderr)
    libs = list(out_lib.glob("*.lib"))
    if not libs:
        print("[V8 vcpkg] Warning: no .lib in output. Check vcpkg v8 port layout.", file=sys.stderr)

    print(f"[V8 vcpkg] Done: {out_include} and {out_lib}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
