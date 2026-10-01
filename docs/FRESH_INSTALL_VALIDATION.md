# Fresh installation validation

Validated on 2026-10-01. The installation target is Pop!_OS 24.04 with the COSMIC
Wayland desktop, a normal user with sudo access, and an internet connection.
See [the installation commands](../README.md#installation).

## Installation behavior

- Missing runtime, graphical Settings, and build packages are installed through
  normal password-based sudo. The full installer runs as the desktop user.
- The compiler check compiles and links C code, detecting missing libc headers
  and startup objects as well as missing compiler executables.
- Both local compositor patches can be committed without a user Git identity
  or signing key. Only authenticated interrupted patch states are recovered.
- The clipboard bridge is rebuilt for the current machine on every install.
- Desktop entries and autostart use absolute commands. App Library launches work
  before the desktop session's PATH includes the new user bin directories.
- Installation reruns preserve settings; managed aliases preserve existing user
  aliases. Package and build failures stop installation before success is reported.

## Real clean build

A disposable rootless environment was created from Canonical's Ubuntu Base
24.04.5 amd64 archive, verified against its published SHA-256 checksum:

`e77b6f10c2590cef872b33ee9f635a0e3fd1f57fb074c0e52b5c7f56147a0c86`

It used the Pop!_OS noble release and Ubuntu package repositories and their
signing keys. Git, Python, and certificates were prepared as base tools; there
was no compiler, development-library setup, Git identity, or vendor build.
Rack's actual scripts then installed their dependencies, cloned the pinned
upstream sources, applied their patches, and built wlroots, Sway, and the bridge.
No host development directories or libraries were mounted into the environment.

The resulting wlroots library passed the Wayland, Pixman, EGL, GLES2, GBM, and DnD
feature checks. Both source trees remained clean. Second bootstrap runs reused
the verified builds, Sway `--check` passed, and a real Wayland client completed
100 popup creation/commit/reposition/destruction cycles using the new build.

The rootless test harness required a package-configuration retry because its
single mapped group could not perform fontconfig's initial `root:staff` chown.
This was a harness restriction; no Rack or system package code was modified.

## Regression results

| Check | Result |
| --- | --- |
| Build dependencies | 13/13 |
| wlroots bootstrap and recovery | 17/17 |
| Sway bootstrap and recovery | 10/10 |
| Fresh-user installation and rerun | 11/11 |
| Desktop launchers, aliases, autostart, startup status | 8/8 |
| Layout-helper lifecycle | 4/4 |
| Acceptance | 41/41 |
| COSMIC Files parity | 29/29 |
| Host-open bridge | 10/10 |
| Four-pane layout without GPU tools | 1/1 |
| Clipboard reliability | 140 swaps; stable descriptors, memory, process count |
| Clipboard offer lifetime | 2,000 cycles; 4,029 offers reclaimed; sanitizer clean |
| DnD lifetime | 240 selections, 480 cancellations, 240 outgoing endings, retained drops and closed-pipe handling passed |
| Real Rack launcher popups | 100 cycles |

The fresh-user installer tests execute the real installer and package helper
with simulated package/build commands and a disposable user profile. Desktop
entry tests also launch through Gio with a minimal PATH and special characters
in the clone path. The real clean build above independently verifies compilation
and dependency completeness.

This does not replace testing the laptop's GPU/driver combination and interactive
COSMIC session. On the laptop, launch normally and check four panes, right-click
menus, clipboard/DnD in both directions, external document opening, and close/reopen.
The normal renderer remains the default; no automatic Pixman fallback is claimed.
