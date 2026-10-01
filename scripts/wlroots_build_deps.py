#!/usr/bin/env python3
"""Check and, on Debian-family systems, install Rack's wlroots build inputs."""

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


PACKAGE_INSTALLER = Path(__file__).resolve().with_name("install-packages.sh")


@dataclass(frozen=True)
class Requirement:
    label: str
    package: str
    kind: str = "tool"
    version: str = ""


# Meson 0.17.4's unconditional dependencies with -Dexamples=false, the
# explicitly required GLES2 renderer/GBM allocator, and bootstrap/bridge tools.
REQUIREMENTS = (
    Requirement("git", "git"),
    Requirement("meson", "meson", "tool", "0.60.0"),
    Requirement("ninja", "ninja-build"),
    Requirement("gcc", "gcc"),
    Requirement("C headers and linker", "libc6-dev", "compile"),
    Requirement("make", "make"),
    Requirement("nm", "binutils"),
    Requirement("pkg-config", "pkg-config"),
    Requirement("wayland-scanner", "libwayland-bin"),
    Requirement("pkg:wayland-server>=1.22", "libwayland-dev", "pc", "1.22"),
    Requirement("pkg:wayland-client", "libwayland-dev", "pc"),
    Requirement("pkg:wayland-scanner", "libwayland-bin", "pc"),
    Requirement("pkg:wayland-protocols>=1.32", "wayland-protocols", "pc", "1.32"),
    Requirement("pkg:xkbcommon", "libxkbcommon-dev", "pc"),
    Requirement("pkg:libdrm>=2.4.114", "libdrm-dev", "pc", "2.4.114"),
    Requirement("pkg:pixman-1>=0.42.0", "libpixman-1-dev", "pc", "0.42.0"),
    Requirement("pkg:egl", "libegl-dev", "pc"),
    Requirement("pkg:gbm>=17.1.0", "libgbm-dev", "pc", "17.1.0"),
    Requirement("pkg:glesv2", "libgles-dev", "pc"),
)
BRIDGE_LABELS = {"make", "gcc", "C headers and linker", "pkg-config", "pkg:wayland-client"}
BRIDGE_REQUIREMENTS = tuple(
    requirement for requirement in REQUIREMENTS if requirement.label in BRIDGE_LABELS
)


def version_at_least(actual: str, minimum: str) -> bool:
    match = re.match(r"^(\d+(?:\.\d+){1,2})", actual.strip())
    if match is None:
        return False
    found = tuple(int(part) for part in match.group(1).split("."))
    needed = tuple(int(part) for part in minimum.split("."))
    return found + (0,) * (3 - len(found)) >= needed + (0,) * (3 - len(needed))


def c_toolchain_works() -> bool:
    """A compiler executable alone does not supply libc headers/startup objects."""
    if shutil.which("gcc") is None:
        return False
    source = """#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <math.h>
int main(void) {
    volatile double value = 2.0;
    printf("%f %lu\\n", sqrt(value), (unsigned long)pthread_self());
    return EXIT_SUCCESS;
}
"""
    try:
        with tempfile.TemporaryDirectory(prefix="sc7-compiler-check-") as temporary:
            result = subprocess.run(
                ["gcc", "-x", "c", "-", "-o", str(Path(temporary) / "probe"),
                 "-lm", "-lrt", "-pthread"],
                input=source, text=True, check=False,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
        return result.returncode == 0
    except OSError:
        return False


def missing_requirements(requirements: tuple[Requirement, ...] = REQUIREMENTS) -> list[Requirement]:
    missing = []
    have_pkg_config = shutil.which("pkg-config") is not None
    for requirement in requirements:
        if requirement.kind == "tool":
            if shutil.which(requirement.label) is None:
                missing.append(requirement)
            elif requirement.version:
                try:
                    result = subprocess.run([requirement.label, "--version"], check=False,
                                            capture_output=True, text=True)
                    if result.returncode != 0 or not version_at_least(
                        result.stdout, requirement.version
                    ):
                        missing.append(requirement)
                except OSError:
                    missing.append(requirement)
        elif requirement.kind == "compile":
            if not c_toolchain_works():
                missing.append(requirement)
        elif have_pkg_config:
            package_name = requirement.label.removeprefix("pkg:").split(">=", 1)[0]
            command = ["pkg-config", "--exists", package_name]
            if requirement.version:
                command = ["pkg-config", f"--atleast-version={requirement.version}", package_name]
            if subprocess.run(command, check=False, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode != 0:
                missing.append(requirement)
    return missing


def apt_packages(requirements: list[Requirement]) -> list[str]:
    packages = list(dict.fromkeys(requirement.package for requirement in requirements))
    # libwayland-dev depends on the scanner package; installing it covers both.
    if "libwayland-dev" in packages and "libwayland-bin" in packages:
        packages.remove("libwayland-bin")
    return packages


def supported_apt_system() -> bool:
    try:
        content = Path("/etc/os-release").read_text(encoding="utf-8")
    except OSError:
        return False
    values = {}
    for line in content.splitlines():
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            values[key] = value.strip().strip('"\'')
    family = {values.get("ID", "").lower(), *values.get("ID_LIKE", "").lower().split()}
    return bool(family & {"debian", "ubuntu", "pop"}) and shutil.which("apt-get") is not None


def show_missing(missing: list[Requirement]) -> None:
    print("  Missing build dependencies found:")
    for requirement in missing:
        minimum = f">={requirement.version}" if requirement.kind == "tool" and requirement.version else ""
        print(f"    - {requirement.label}{minimum}")


def ensure_build_dependencies(install: bool, bridge_only: bool = False) -> int:
    print("Checking clipboard bridge build dependencies..." if bridge_only else
          "Checking wlroots build dependencies...")
    requirements = BRIDGE_REQUIREMENTS if bridge_only else REQUIREMENTS
    missing = missing_requirements(requirements) if bridge_only else missing_requirements()
    if not missing:
        print("  ✓ Build dependencies ready")
        return 0
    show_missing(missing)
    if not install:
        return 1
    if not supported_apt_system():
        print("  Automatic installation is available only on Debian, Ubuntu, and Pop!_OS with apt-get.",
              file=sys.stderr)
        print("  Install the missing dependencies above with your distribution's package manager, then rerun ./install.sh.",
              file=sys.stderr)
        return 1

    # If pkg-config is missing, install it first so the library probes can
    # determine the exact second set rather than guessing at every package.
    allow_second_stage = any(requirement.label == "pkg-config" for requirement in missing)
    for attempt in range(2 if allow_second_stage else 1):
        packages = apt_packages(missing)
        if not packages:
            break
        command = [str(PACKAGE_INSTALLER), *packages]
        print("Installing required SC7Labs Rack build dependencies: " + " ".join(packages), flush=True)
        if subprocess.run(command, check=False).returncode != 0:
            print("  Package installation failed; fix the apt error above and rerun ./install.sh.",
                  file=sys.stderr)
            return 1
        missing = missing_requirements(requirements) if bridge_only else missing_requirements()
        if not missing:
            print("  ✓ Build dependencies ready")
            return 0
        show_missing(missing)
        if attempt != 0 or not allow_second_stage or not any(
            requirement.kind == "pc" for requirement in missing
        ):
            break

    print("  Required build dependencies are still missing after installation.", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="check only")
    group.add_argument("--ensure", action="store_true", help="install missing apt packages when supported")
    parser.add_argument("--bridge-only", action="store_true", help="check only clipboard bridge inputs")
    args = parser.parse_args()
    return ensure_build_dependencies(install=args.ensure, bridge_only=args.bridge_only)


if __name__ == "__main__":
    raise SystemExit(main())
