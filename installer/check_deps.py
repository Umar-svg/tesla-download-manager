#!/usr/bin/env python3
"""Verifies that every DLL imported by the staged executables and DLLs can be
resolved on a clean Windows 10/11 machine: either it ships in the install tree
(bin/ or a plugin's own folder) or it is a Windows system DLL that is
guaranteed to exist (System32 / API sets). Uses dumpbin /dependents.

  python check_deps.py <stage dir>   (run from a VS developer environment)

Exit 0 = all imports resolvable; 1 = something would be missing on a clean PC.
"""
import os
import subprocess
import sys

# DLLs that are part of every supported Windows (10/11) installation. The
# MSVC runtime and the UCRT DLLs are *not* listed here except the UCRT, which
# has been an OS component since Windows 10.
SYSTEM_PREFIXES = ("api-ms-win-", "ext-ms-win-")
SYSTEM32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
# Present in System32 on a developer box but NOT guaranteed on a clean PC:
NOT_SYSTEM = {"msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll",
              "concrt140.dll", "libssl-3-x64.dll", "libcrypto-3-x64.dll", "sqlite3.dll"}


def imports(path):
    out = subprocess.run(["dumpbin", "/nologo", "/dependents", path], capture_output=True, text=True).stdout
    deps, on = [], False
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("Image has the following dependencies"):
            on = True
            continue
        if on:
            if not s:
                if deps:
                    break
                continue
            if s.lower().endswith(".dll"):
                deps.append(s)
            elif s.startswith("Summary"):
                break
    return deps


def main():
    stage = os.path.abspath(sys.argv[1])
    binaries = []
    for d, _, files in os.walk(stage):
        binaries += [os.path.join(d, f) for f in files if f.lower().endswith((".exe", ".dll"))]
    shipped = {f.lower() for f in os.listdir(os.path.join(stage, "bin"))}
    problems = []
    checked = 0
    for b in sorted(binaries):
        for dep in imports(b):
            checked += 1
            low = dep.lower()
            if low in shipped or low.startswith(SYSTEM_PREFIXES):
                continue
            if low in NOT_SYSTEM:
                problems.append(f"{os.path.relpath(b, stage)} -> {dep} (must be shipped, not in bin/)")
                continue
            if os.path.exists(os.path.join(SYSTEM32, dep)):
                continue
            problems.append(f"{os.path.relpath(b, stage)} -> {dep} (not shipped, not a system DLL)")
    print(f"checked {checked} imports of {len(binaries)} binaries")
    for p in problems:
        print("MISSING:", p)
    system_shipped = [f for f in shipped if f.startswith("icu")]
    for f in system_shipped:
        print("SHIPS A SYSTEM DLL:", f)
    return 1 if problems or system_shipped else 0


if __name__ == "__main__":
    sys.exit(main())
