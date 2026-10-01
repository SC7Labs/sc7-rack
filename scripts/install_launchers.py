#!/usr/bin/env python3
"""Install desktop launchers that work in the current desktop session."""

import argparse
import os
from pathlib import Path
import re
import shlex


ROOT = Path(__file__).resolve().parents[1]
ALIAS_START = "# >>> SC7Labs Rack managed alias >>>"
ALIAS_END = "# <<< SC7Labs Rack managed alias <<<"
ALIAS_BLOCK = re.compile(
    r"(?m)^" + re.escape(ALIAS_START) + r"\n.*?^" + re.escape(ALIAS_END) + r"\n?",
    re.DOTALL,
)


def desktop_exec(arguments):
    """Encode arguments through both Desktop Entry string and Exec quoting."""
    arguments = list(arguments)
    # '=' is forbidden in an Exec program name. Gio also checks an executable
    # containing '%%' before expanding it. Pass those paths as literal arguments
    # to a fixed shell program; no path text becomes shell code.
    if arguments and any(char in str(arguments[0]) for char in "=%"):
        arguments = ["/bin/sh", "-c", 'exec "$@"', "sc7-rack", *arguments]
    quoted = []
    for argument in arguments:
        value = str(argument).replace("%", "%%")
        # Exec quoting is decoded after the desktop file's string escapes.
        value = "".join("\\" + char if char in '\\"`$' else char for char in value)
        value = (value.replace("\\", "\\\\").replace("\n", "\\n")
                 .replace("\r", "\\r").replace("\t", "\\t"))
        quoted.append('"' + value + '"')
    return " ".join(quoted)


def render_launcher(template, root):
    lines = []
    for line in template.splitlines(keepends=True):
        if line.startswith("Exec="):
            arguments = shlex.split(line.removeprefix("Exec="))
            if not arguments or arguments[0] not in {"sc7-rack", "sc7-rack-settings"}:
                raise ValueError("Unexpected command in Rack desktop template")
            arguments[0] = root / "bin" / arguments[0]
            line = "Exec=" + desktop_exec(arguments) + "\n"
        lines.append(line)
    return "".join(lines)


def install_launchers():
    data_home = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    applications = data_home / "applications"
    applications.mkdir(parents=True, exist_ok=True)
    for name in ("sc7-rack.desktop", "sc7-rack-settings.desktop"):
        content = render_launcher((ROOT / "desktop" / name).read_text(), ROOT)
        destination = applications / name
        destination.write_text(content)
        destination.chmod(0o644)
    app_id = applications / "dev.sc7labs.rack.desktop"
    if app_id.is_symlink() or app_id.exists():
        app_id.unlink()
    app_id.symlink_to("sc7-rack.desktop")


def update_shell_aliases(remove=False):
    command = shlex.quote(str(ROOT / "bin/sc7-rack"))
    block = f"{ALIAS_START}\nalias rack={shlex.quote(command)}\n{ALIAS_END}\n"
    for filename in (".bashrc", ".zshrc"):
        rc = Path.home() / filename
        if not rc.is_file():
            continue
        current = rc.read_text()
        if ALIAS_BLOCK.search(current):
            updated = ALIAS_BLOCK.sub(lambda match: "" if remove else block, current)
        elif remove or re.search(r"(?m)^\s*alias\b[^\n]*\brack\s*=", current):
            continue
        else:
            updated = current + ("" if not current or current.endswith("\n") else "\n") + block
        if updated != current:
            rc.write_text(updated)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remove-aliases", action="store_true")
    args = parser.parse_args()
    if not args.remove_aliases:
        install_launchers()
    update_shell_aliases(remove=args.remove_aliases)
