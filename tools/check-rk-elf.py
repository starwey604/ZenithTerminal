#!/usr/bin/env python3
"""Check an AArch64 application's loader and versioned imports against its SDK."""

import argparse
import os
import re
import subprocess
from pathlib import Path


def readelf(path, option):
    return subprocess.check_output(
        ["readelf", option, str(path)], text=True, env=dict(os.environ, LC_ALL="C")
    )


def in_sysroot(root, path):
    """Resolve target-absolute symlinks inside the target root, never the host root."""
    todo = list(Path(path).parts)
    resolved = root
    hops = 0
    while todo:
        component = todo.pop(0)
        if component == "/":
            resolved = root
        elif component == "..":
            if resolved == root:
                raise ValueError("symlink escapes sysroot")
            resolved = resolved.parent
        elif component != ".":
            resolved /= component
            if resolved.is_symlink():
                hops += 1
                if hops > 40:
                    raise ValueError("symlink loop in sysroot")
                link = resolved.readlink()
                resolved = resolved.parent
                todo = list(link.parts) + todo
    return resolved


def check(binary, sdk):
    root = sdk / "aarch64-buildroot-linux-gnu/sysroot"
    header = readelf(binary, "-h")
    if not re.search(r"Machine:\s+AArch64", header):
        raise ValueError("application is not AArch64")
    program = readelf(binary, "-l")
    match = re.search(r"Requesting program interpreter: (.*?)\]", program)
    if not match or match[1] != "/lib/ld-linux-aarch64.so.1":
        raise ValueError("unexpected dynamic loader")
    loader = in_sysroot(root, match[1])
    if not loader.is_file():
        raise ValueError(f"loader missing from SDK: {match[1]}")
    if not re.search(r"Machine:\s+AArch64", readelf(loader, "-h")):
        raise ValueError("SDK loader is not AArch64")
    dynamic = readelf(binary, "-d")
    if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
        raise ValueError("application contains RPATH/RUNPATH")
    libraries = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
    providers = {}
    for name in libraries:
        for directory in ["lib", "lib64", "usr/lib", "usr/lib64"]:
            candidate = in_sysroot(root, f"{directory}/{name}")
            if candidate.is_file():
                if not re.search(r"Machine:\s+AArch64", readelf(candidate, "-h")):
                    raise ValueError(f"SDK library is not AArch64: {candidate}")
                providers[name] = candidate
                break
        else:
            raise ValueError(f"dependency missing from SDK: {name}")

    # Match each requested symbol version to the specific provider named by ELF.
    version_info = readelf(binary, "-V")
    provider = None
    requirements = []
    for line in version_info.splitlines():
        file_match = re.search(r"File: (\S+)", line)
        if file_match:
            provider = file_match[1]
        version_match = re.search(r"Name: (\S+)", line)
        if provider and version_match:
            requirements.append((provider, version_match[1]))
    available = {}
    for name, path in providers.items():
        definitions = readelf(path, "-V").partition("Version definition section")[2]
        definitions = definitions.partition("Version needs section")[0]
        available[name] = set(re.findall(r"Name: (\S+)", definitions))
    for name, version in requirements:
        if version not in available.get(name, set()):
            raise ValueError(f"SDK does not provide {name}: {version}")
    print(f"ELF CHECK PASS: {binary}")
    print(f"  machine: AArch64; interpreter: {match[1]}; no RPATH/RUNPATH")
    print(f"  SDK dependencies: {', '.join(libraries)}")
    print(f"  matched version requirements: {len(requirements)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--sdk-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        check(args.binary.resolve(), args.sdk_root.resolve())
    except (ValueError, subprocess.CalledProcessError, OSError) as error:
        parser.exit(1, f"ELF CHECK FAIL: {error}\n")
