#!/usr/bin/env bash
# Install requested build/runtime inputs; package management is the only
# privileged step. The calling installer continues as the desktop user.
set -euo pipefail

die() { echo "SC7 Rack packages: $*" >&2; exit 1; }
(( $# > 0 )) || exit 0
for package in "$@"; do
    [[ "$package" =~ ^[a-z0-9][a-z0-9+.-]*(:[a-z0-9-]+)?$ ]] ||
        die "Invalid package name: $package"
done

[[ -r /etc/os-release ]] || die "Cannot identify this operating system"
# Distribution-owned metadata, not user input.
source /etc/os-release
case " ${ID:-} ${ID_LIKE:-} " in
    *" pop "*|*" ubuntu "*|*" debian "*) ;;
    *) die "Automatic package installation requires Pop!_OS, Ubuntu, or Debian" ;;
esac
command -v apt-get >/dev/null 2>&1 || die "apt-get is unavailable"

privilege=()
if [[ "$(id -u)" != 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die "sudo is required to install: $*"
    privilege=(sudo)
fi
echo "SC7 Rack packages: installing $*"
"${privilege[@]}" apt-get update ||
    die "Could not refresh package lists. Fix the apt error above and rerun ./install.sh"
"${privilege[@]}" apt-get install -y --no-install-recommends "$@" ||
    die "Package installation failed. Fix the apt error above and rerun ./install.sh"
