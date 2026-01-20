#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 UPBGE
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Downloads V8 (headers + .lib + .dll) from NuGet packages v8-v143-x64 and
# v8.redist-v143-x64, and extracts to lib/windows_x64/v8/{include,lib}.
# Requires Python 3.6+.
#
# Usage (from project root or with --output):
#   python build_files/download_v8_nuget.py
#   python build_files/download_v8_nuget.py --output lib/windows_x64/v8
#
# Optional: --toolset v142 (VS 2019) instead of v143 (VS 2022), --version X.Y.Z.W

import argparse
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

try:
    from urllib.request import urlopen, Request
    from urllib.error import URLError, HTTPError
except ImportError:
    from urllib2 import urlopen, Request, URLError, HTTPError  # type: ignore

NUGET_BASE = "https://api.nuget.org/v3-flatcontainer"
# VS 2022 (v143) by default; packages v8-v143-x64 and v8.redist-v143-x64
DEFAULT_TOOLSET = "v143"
# 13.x is newer; 11.x can have better MSVC/header compatibility if 13.x fails.
# Note: 11.0.226.19 exists for v142 only; for v143 use 11.9.169.4.
DEFAULT_VERSION = "13.0.245.25"
VERSION_11_V142 = "11.0.226.19"   # VS 2019
VERSION_11_V143 = "11.9.169.4"    # VS 2022 (v143; 11.0.x not published for v143)


def find_project_root() -> Path:
    root = Path(__file__).resolve().parent.parent
    if (root / "CMakeLists.txt").exists() and (root / "build_files").exists():
        return root
    return Path.cwd()


def download(url: str, dest: Path) -> None:
    req = Request(url, headers={"User-Agent": "UPBGE-build"})
    with urlopen(req, timeout=120) as resp:
        dest.write_bytes(resp.read())


def unpack_nupkg(nupkg: Path, out: Path) -> None:
    with zipfile.ZipFile(nupkg, "r") as z:
        z.extractall(out)


def find_include(root: Path) -> Path | None:
    for p in root.rglob("v8.h"):
        return p.parent
    return None


def find_all_libs(root: Path) -> list[Path]:
    return list(root.rglob("*.lib"))


def find_dlls(root: Path) -> Path | None:
    for p in root.rglob("*.dll"):
        return p.parent
    return None


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Download V8 (NuGet) and extract to lib/windows_x64/v8 (include + lib)."
    )
    ap.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output directory (e.g. lib/windows_x64/v8). Default: <root>/lib/windows_x64/v8",
    )
    ap.add_argument(
        "--toolset",
        default=DEFAULT_TOOLSET,
        choices=("v142", "v143"),
        help="MSVC toolset: v142=VS2019, v143=VS2022 (default)",
    )
    ap.add_argument(
        "--version",
        default=DEFAULT_VERSION,
        help=f"NuGet package version (default: {DEFAULT_VERSION})",
    )
    ap.add_argument(
        "--no-redist",
        action="store_true",
        help="Do not download v8.redist (headers and .lib only)",
    )
    ap.add_argument(
        "--version-11",
        action="store_true",
        help="Use V8 11.x (11.9.169.4 for v143, 11.0.226.19 for v142); can help if 13.x has header errors",
    )
    args = ap.parse_args()
    if args.version_11:
        args.version = VERSION_11_V143 if args.toolset == "v143" else VERSION_11_V142

    root = find_project_root()
    out = args.output or (root / "lib" / "windows_x64" / "v8")
    out = out.resolve()
    out_include = out / "include"
    out_lib = out / "lib"

    tool = args.toolset
    ver = args.version
    pkg_dev = f"v8-{tool}-x64"
    pkg_redist = f"v8.redist-{tool}-x64"

    # NuGet package IDs are lowercase
    url_dev = f"{NUGET_BASE}/{pkg_dev}/{ver}/{pkg_dev}.{ver}.nupkg"
    url_redist = f"{NUGET_BASE}/{pkg_redist}/{ver}/{pkg_redist}.{ver}.nupkg"

    print(f"[V8] Output: {out}")
    print(f"[V8] Packages: {pkg_dev} and {pkg_redist} @ {ver}")

    with tempfile.TemporaryDirectory(prefix="v8_nuget_") as tmp:
        t = Path(tmp)

        # 1) Developer (headers + .lib)
        nupkg_dev = t / f"{pkg_dev}.nupkg"
        try:
            print(f"[V8] Downloading {pkg_dev}...")
            download(url_dev, nupkg_dev)
        except (URLError, HTTPError) as e:
            print(f"[V8] Error downloading {pkg_dev}: {e}", file=sys.stderr)
            return 1

        unpack_nupkg(nupkg_dev, t / "dev")
        dev_root = t / "dev"

        inc_src = find_include(dev_root)
        if not inc_src:
            print("[V8] Error: v8.h not found in the developer package.", file=sys.stderr)
            return 1

        libs = find_all_libs(dev_root)
        if not libs:
            print("[V8] Error: no .lib found in the developer package.", file=sys.stderr)
            return 1

        out.mkdir(parents=True, exist_ok=True)
        if out_include.exists():
            shutil.rmtree(out_include)
        if out_lib.exists():
            shutil.rmtree(out_lib)
        out_include.mkdir(parents=True)
        out_lib.mkdir(parents=True)

        # Copy include tree (v8.h may be in include/ or include/v8/ etc.)
        for f in inc_src.rglob("*"):
            if f.is_file():
                rel = f.relative_to(inc_src)
                dst = out_include / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, dst)
        for f in libs:
            shutil.copy2(f, out_lib)

        # 2) Redist (.dll)
        if not args.no_redist:
            nupkg_redist = t / f"{pkg_redist}.nupkg"
            try:
                print(f"[V8] Downloading {pkg_redist}...")
                download(url_redist, nupkg_redist)
            except (URLError, HTTPError) as e:
                print(f"[V8] Warning: could not download {pkg_redist}: {e}", file=sys.stderr)
            else:
                unpack_nupkg(nupkg_redist, t / "redist")
                redist_root = t / "redist"
                dll_src = find_dlls(redist_root)
                if dll_src:
                    for f in dll_src.glob("*.dll"):
                        shutil.copy2(f, out_lib)
                    print(f"[V8] DLLs copied to {out_lib}")
                else:
                    print("[V8] Warning: no .dll in redist package.", file=sys.stderr)

    print(f"[V8] Done: {out_include} and {out_lib}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
